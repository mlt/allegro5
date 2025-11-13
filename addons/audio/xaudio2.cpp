#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#define NO_GDI
#include <windows.h>

#include <memory> // unique_ptr
/* This is for the ComPtr
 * In case of emergency, use bare pointer for device */
#define __WRL_NO_DEFAULT_LIB__
#define __WRL_CLASSIC_COM_STRICT__
#include <wrl/client.h>
using namespace Microsoft::WRL;

#include <xaudio2.h>
#ifndef XAUDIO2_VOICE_NOSAMPLESPLAYED
   #error XAudio2 headers are too old (<2.8). Please remove DirectX SDK.
#endif

#define ALLEGRO_INTERNAL_UNSTABLE
#include "allegro5/allegro.h"

ALLEGRO_DEBUG_CHANNEL("XAudio2")

extern "C" {
#include "allegro5/internal/aintern_audio.h"
// #include "allegro5/platform/aintwin.h" // for _al_win_thread_init
}

/* XAudio2 vars */
static ComPtr<IXAudio2> device;
static char err_str[100];
static unsigned int buffer_size = 512; // in samples
static unsigned int buffers_count = 2;

#define MIN_BUFFER_SIZE    32
#define MIN_BUFFERS_COUNT    2

#define CASE_XAUDIO2_ERROR(err) case err: strcpy(err_str, #err); break

static char *get_error(HRESULT hr)
{
   switch (hr) {
      CASE_XAUDIO2_ERROR(XAUDIO2_E_INVALID_CALL);
      CASE_XAUDIO2_ERROR(XAUDIO2_E_XMA_DECODER_ERROR);
      CASE_XAUDIO2_ERROR(XAUDIO2_E_XAPO_CREATION_FAILED);
      CASE_XAUDIO2_ERROR(XAUDIO2_E_DEVICE_INVALIDATED);
      CASE_XAUDIO2_ERROR(CO_E_NOTINITIALIZED);
      default: sprintf(err_str, "Unknown error 0x%08lx", hr);
   }

   return err_str;
}

IXAudio2MasteringVoice *mastering_voice = NULL;

class VoiceCallback;

/* Custom struct to hold voice information XAudio2 needs */
struct ALLEGRO_XAUDIO2_DATA {
   int bits_per_sample;
   int channels;
   IXAudio2SourceVoice *voice;
   WAVEFORMATEXTENSIBLE wave_fmt;
   XAUDIO2_BUFFER buffer;
   int stop_voice;
   std::unique_ptr<VoiceCallback> callback;
   HANDLE thread;
};

#define BUFFER_END 0
#define WANT_QUIT 1
#define BUFFER_END_EVENT events[BUFFER_END]
#define WANT_QUIT_EVENT events[WANT_QUIT]

class VoiceCallback : public IXAudio2VoiceCallback
{
   ALLEGRO_VOICE *voice; // to set stream_end
   VoiceCallback() = delete;
public:
   HANDLE events[2]; /* buffer end, want quit */
   VoiceCallback(ALLEGRO_VOICE *v) : voice(v),
      events{ CreateEvent(NULL, FALSE, FALSE, NULL), CreateEvent(NULL, FALSE, FALSE, NULL) } {}
   ~VoiceCallback() { CloseHandle(BUFFER_END_EVENT); CloseHandle(WANT_QUIT_EVENT); }
protected:
    STDMETHOD_(void, OnVoiceProcessingPassStart) (THIS_ UINT32 BytesRequired) override {
        (void)BytesRequired;
        //ALLEGRO_DEBUG("OnVoiceProcessingPassStart called, BytesRequired=%u\n", BytesRequired); // 1688
    }
    STDMETHOD_(void, OnVoiceProcessingPassEnd) (THIS) override {
       //ALLEGRO_DEBUG("OnVoiceProcessingPassEnd called\n");
    }
    STDMETHOD_(void, OnStreamEnd) (THIS) override {
       ALLEGRO_DEBUG("OnStreamEnd called\n");
       ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;
       ex_data->stop_voice = 1;
    }
    STDMETHOD_(void, OnBufferStart) (THIS_ void* pBufferContext) override {
       //ALLEGRO_DEBUG("OnBufferStart called\n");
#if 0
       HRESULT hr;
       //ALLEGRO_VOICE *const voice = (ALLEGRO_VOICE *)pBufferContext;
       ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;
       const int bytes_per_sample = ex_data->bits_per_sample / 8;
       unsigned int samples = buffer_size;// / bytes_per_sample / ex_data->channels;
       const void* data = _al_voice_update(voice, voice->mutex, &samples);
       if (!data) {
          hr = ex_data->voice->Discontinuity();
          if (FAILED(hr)) {
               ALLEGRO_ERROR("Discontinuity failed: %s\n", get_error(hr));
               return;
          }
       }
       ex_data->buffer.AudioBytes = samples * bytes_per_sample * ex_data->channels;
       ex_data->buffer.pAudioData = (const BYTE *)data;
       hr = ex_data->voice->SubmitSourceBuffer(&ex_data->buffer, NULL);
       if (FAILED(hr)) {
          ALLEGRO_DEBUG("SubmitSourceBuffer failed: %s\n", get_error(hr));
       }
#endif
    }
    STDMETHOD_(void, OnBufferEnd) (THIS_ void* pBufferContext) override {
        (void)pBufferContext;
        //ALLEGRO_DEBUG("OnBufferEnd called\n");
       SetEvent(BUFFER_END_EVENT);
    }
    STDMETHOD_(void, OnLoopEnd) (THIS_ void* pBufferContext) override {
        (void)pBufferContext;
        ALLEGRO_DEBUG("OnLoopEnd called\n");
    }
    STDMETHOD_(void, OnVoiceError) (THIS_ void* pBufferContext, HRESULT Error) override {
        ALLEGRO_ERROR("OnVoiceError called, Error=%s\n", get_error(Error));
    }
};

static DWORD WINAPI _xaudio2_update(LPVOID lpThreadParameter)
{
   ALLEGRO_VOICE *voice = (ALLEGRO_VOICE *)lpThreadParameter;
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;
   const int bytes_per_sample = ex_data->bits_per_sample >> 3;
   const int buffer_size_bytes = buffer_size * bytes_per_sample * ex_data->channels;
   LPBYTE silence = (LPBYTE)malloc(buffer_size_bytes);
   while (!ex_data->stop_voice) {
#if 0
#else
      XAUDIO2_VOICE_STATE state;
      while (ex_data->voice->GetState(&state), state.BuffersQueued > buffers_count - 1)
      {
         switch (WaitForMultipleObjects(2, ex_data->callback->events, FALSE, INFINITE) - WAIT_OBJECT_0)
         {
            case BUFFER_END: break;
            case WANT_QUIT:
               /* Although we don't have fancy C++ objects here but use return hence C++.
                * Either use C for ExitThread or C++ threads.
                * We are on Windows after all so no need for al_create_thread
                */
               goto exit;
         default:
            ALLEGRO_ERROR("WaitForMultipleObjects failed in xaudio2 update thread\n");
            goto exit;
         }
      }
#endif
      //al_wait_cond(voice->cond, voice->mutex);
      unsigned int samples = buffer_size;// / bytes_per_sample / ex_data->channels;
      const void* data = _al_voice_update(voice, voice->mutex, &samples);
      if (data) {
         ex_data->buffer.AudioBytes = samples * bytes_per_sample *ex_data->channels;
         ex_data->buffer.pAudioData = (const BYTE *)data;
      } else {
#if 1
         ex_data->buffer.AudioBytes = buffer_size_bytes;
         ex_data->buffer.pAudioData = silence;
#else
         HRESULT hr = ex_data->voice->Discontinuity();
         if (FAILED(hr)) {
              ALLEGRO_ERROR("Discontinuity failed: %s\n", get_error(hr));
              break;
         }
#endif
      }
      //ALLEGRO_DEBUG("Feeding audio buffer\n");
      HRESULT hr = ex_data->voice->SubmitSourceBuffer(&ex_data->buffer, NULL);
      if (FAILED(hr)) {
         ALLEGRO_DEBUG("SubmitSourceBuffer failed: %s\n", get_error(hr));
         //break;
      }
   }

exit:
   al_free(silence);
   return 0;
}

#define REPORT_FAILED_1(expr) do { \
   HRESULT hr = expr; \
   if (FAILED(hr)) { \
      ALLEGRO_ERROR(#expr " failed: %s\n", get_error(hr)); \
      return 1; \
   } } while(0)


#define CONFIG_INT(var, min) do {                                                     \
   const char *val = al_get_config_value(al_get_system_config(), "xaudio2", #var);    \
   if (val && val[0] != '\0') {                                                       \
      var = atoi(val); if (var < min) var = min;                                      \
   }} while (0)

static int com_initialized = 0;

/* The open method starts up the driver and should lock the device, using the
   previously set parameters, or defaults. It shouldn't need to start sending
   audio data to the device yet, however. */
static int _xaudio2_open()
{
   ALLEGRO_INFO("Starting XAudio2...\n");
   if (!com_initialized) {
      // _al_win_thread_init();
      (void)CoInitializeEx(NULL, COINIT_MULTITHREADED);
      com_initialized = 1;
   }

   IXAudio2 *raw_device = NULL;
   REPORT_FAILED_1( XAudio2Create(&raw_device, 0, XAUDIO2_DEFAULT_PROCESSOR) );
   device = raw_device;
   REPORT_FAILED_1( device->CreateMasteringVoice(&mastering_voice) );

   CONFIG_INT(buffer_size, MIN_BUFFER_SIZE);
   CONFIG_INT(buffers_count, MIN_BUFFERS_COUNT);

   ALLEGRO_DEBUG("XAudio2Create succeeded\n");

   return 0;
}


/* The close method should close the device, freeing any resources, and allow
   other processes to use the device */
static void _xaudio2_close()
{
   ALLEGRO_DEBUG("Releasing device\n");

   mastering_voice->DestroyVoice();
   device = nullptr; // destroy if any

   ALLEGRO_DEBUG("Released device\n");
   if (com_initialized) {
      CoUninitialize();
      com_initialized = 0;
   }
   ALLEGRO_INFO("XAudio2 closed\n");
}


/* The allocate_voice method should grab a voice from the system, and allocate
   any data common to streaming and non-streaming sources. */
static int _xaudio2_allocate_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_XAUDIO2_DATA *ex_data;
   int bits_per_sample;
   int channels;

   ALLEGRO_DEBUG("Allocating voice\n");

   /* openal doesn't support very much! */
   switch (voice->depth)
   {
      case ALLEGRO_AUDIO_DEPTH_UINT8:
         /* format supported */
         bits_per_sample = 8;
         break;
      case ALLEGRO_AUDIO_DEPTH_INT8:
         ALLEGRO_ERROR("XAudio2 requires 8-bit data to be unsigned\n");
         return 1;
      case ALLEGRO_AUDIO_DEPTH_UINT16:
         ALLEGRO_ERROR("XAudio2 requires 16-bit data to be signed\n");
         return 1;
      case ALLEGRO_AUDIO_DEPTH_INT16:
         /* format supported */
         bits_per_sample = 16;
         break;
      case ALLEGRO_AUDIO_DEPTH_UINT24:
         ALLEGRO_ERROR("XAudio2 requires 24-bit data to be signed\n");
         return 1;
      case ALLEGRO_AUDIO_DEPTH_INT24:
         /* format supported */
         bits_per_sample = 24;
         break;
      case ALLEGRO_AUDIO_DEPTH_FLOAT32:
         /* format supported */
         bits_per_sample = 32;
         break;
      default:
         ALLEGRO_ERROR("Cannot allocate unknown voice depth\n");
         return 1;
   }

   channels = (int)al_get_channel_count(voice->chan_conf);

   ex_data = (ALLEGRO_XAUDIO2_DATA *)al_calloc(1, sizeof(*ex_data));
   if (!ex_data) {
      ALLEGRO_ERROR("Could not allocate voice data memory\n");
      return 1;
   }

   ex_data->bits_per_sample = bits_per_sample;
   ex_data->channels = channels;
   ex_data->stop_voice = 1;

   voice->extra = ex_data;

   ALLEGRO_DEBUG("Allocated voice\n");

   return 0;
}

/* The deallocate_voice method should free the resources for the given voice,
   but still retain a hold on the device. The voice should be stopped and
   unloaded by the time this is called */
static void _xaudio2_deallocate_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_DEBUG("Deallocating voice\n");

   al_free(voice->extra);
   voice->extra = NULL;

   ALLEGRO_DEBUG("Deallocated voice\n");
}

/* The load_voice method loads a sample into the driver's memory. The voice's
   'streaming' field will be set to false for these voices, and it's
   'buffer_size' field will be the total length in bytes of the sample data.
   The voice's attached sample's looping mode should be honored, and loading
   must fail if it cannot be. */
static int _xaudio2_load_voice(ALLEGRO_VOICE *voice, const void *data)
{
   ALLEGRO_ASSERT(!voice->is_streaming);
   ALLEGRO_ASSERT(voice->buffer_size > 0);
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;
   HRESULT hr;

   ALLEGRO_DEBUG("Loading voice\n");

   ex_data->wave_fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
   ex_data->wave_fmt.Format.nChannels = ex_data->channels;
   ex_data->wave_fmt.Format.nSamplesPerSec = voice->frequency;
   ex_data->wave_fmt.Format.nBlockAlign = ex_data->channels * (ex_data->bits_per_sample >> 3);
   ex_data->wave_fmt.Format.nAvgBytesPerSec = ex_data->wave_fmt.Format.nBlockAlign * voice->frequency;
   ex_data->wave_fmt.Format.wBitsPerSample = ex_data->bits_per_sample;
   ex_data->wave_fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
   ex_data->wave_fmt.dwChannelMask = 0;
   ex_data->wave_fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
   ex_data->wave_fmt.Samples.wValidBitsPerSample = ex_data->bits_per_sample;

   hr = device->CreateSourceVoice(&ex_data->voice, (WAVEFORMATEX *)&ex_data->wave_fmt,
      0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("CreateSourceVoice failed: %s\n", get_error(hr));
      return 1;
   }

   ex_data->buffer.AudioBytes = (UINT32)voice->buffer_size;
   ex_data->buffer.pAudioData = (BYTE *)data;
   ex_data->buffer.Flags = XAUDIO2_END_OF_STREAM;
   if (voice->attached_stream->loop == ALLEGRO_PLAYMODE_LOOP) {
      ex_data->buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
   }
   else {
      ex_data->buffer.LoopCount = 0;
   }
   REPORT_FAILED_1( ex_data->voice->SubmitSourceBuffer(&ex_data->buffer) );

   return 0;
}

/* The unload_voice method unloads a sample previously loaded with load_voice.
   This method should not be called on a streaming voice. */
static void _xaudio2_unload_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;

   ALLEGRO_DEBUG("Unloading voice\n");

   ex_data->voice->DestroyVoice();

   ALLEGRO_DEBUG("Unloaded voice\n");
}


/* The start_voice should, surprise, start the voice. For streaming voices, it
   should start polling the device and call _al_voice_update for audio data.
   For non-streaming voices, it should resume playing from the last set
   position */
static int _xaudio2_start_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;
   HRESULT hr;

   ALLEGRO_DEBUG("Starting voice\n");

   if (!voice->is_streaming) {
      REPORT_FAILED_1( ex_data->voice->Start() );
      ALLEGRO_INFO("Streaming voice started\n");
      return 0;
   }

   if (ex_data->stop_voice != 0) {
      ex_data->wave_fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      ex_data->wave_fmt.Format.nChannels = ex_data->channels;
      ex_data->wave_fmt.Format.nSamplesPerSec = voice->frequency;
      ex_data->wave_fmt.Format.nBlockAlign = ex_data->channels * (ex_data->bits_per_sample >> 3);
      ex_data->wave_fmt.Format.nAvgBytesPerSec = ex_data->wave_fmt.Format.nBlockAlign * voice->frequency;
      ex_data->wave_fmt.Format.wBitsPerSample = ex_data->bits_per_sample;
      ex_data->wave_fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
      ex_data->wave_fmt.dwChannelMask = 0;
      ex_data->wave_fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      ex_data->wave_fmt.Samples.wValidBitsPerSample = ex_data->bits_per_sample;

      ex_data->stop_voice = 0;
   }
   else {
      ALLEGRO_WARN("stop_voice == 0\n");
   }

   memset(&ex_data->buffer, 0, sizeof(ex_data->buffer));

#if 0
   hr = device->CreateSourceVoice(&ex_data->voice, (WAVEFORMATEX *)&ex_data->wave_fmt,
      0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
#else
   ex_data->callback = std::make_unique<VoiceCallback>(voice);
   hr = device->CreateSourceVoice(&ex_data->voice, (WAVEFORMATEX *)&ex_data->wave_fmt,
      0, XAUDIO2_DEFAULT_FREQ_RATIO, static_cast<IXAudio2VoiceCallback *>(ex_data->callback.get()), NULL, NULL);
#endif
   if (FAILED(hr)) {
      ALLEGRO_ERROR("CreateSourceVoice failed: %s\n", get_error(hr));
      return 1;
   }


   REPORT_FAILED_1( ex_data->voice->Start() );

   ex_data->thread = CreateThread(NULL, 0, _xaudio2_update, (LPVOID)voice, 0, NULL);
   //SetThreadPriority(ex_data->thread, THREAD_PRIORITY_LOWEST);
#ifdef DEBUGMODE
   SetThreadDescription(ex_data->thread, L"Allegro XAudio2 updater");
#endif

   ALLEGRO_INFO("Voice started\n");
   return 0;
}

/* The stop_voice method should stop playback. For non-streaming voices, it
   should leave the data loaded, and reset the voice position to 0. */
static int _xaudio2_stop_voice(ALLEGRO_VOICE* voice)
{
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;

   ALLEGRO_DEBUG("Stopping voice\n");

   if (!ex_data->voice) {
      ALLEGRO_ERROR("Trying to stop empty voice\n");
      return 1;
   }

   SetEvent(ex_data->callback->WANT_QUIT_EVENT);
   WaitForSingleObject(ex_data->thread, INFINITE);
   CloseHandle(ex_data->thread);
   REPORT_FAILED_1( ex_data->voice->Stop() );
   REPORT_FAILED_1( ex_data->voice->FlushSourceBuffers() );
   ex_data->callback.reset();
#if 0
   /* if playing a sample */
   if (!voice->is_streaming) {
      ALLEGRO_DEBUG("Stopping non-streaming voice\n");
      ex_data->voice->Stop();
      ex_data->voice->FlushSourceBuffers();
      //ex_data->buffer.pAudioDataSetCurrentPosition(0);
      ALLEGRO_INFO("Non-streaming voice stopped\n");
      return 0;
   }
#endif

   if (ex_data->stop_voice == 0) {
      ex_data->stop_voice = 1;
   }

   ALLEGRO_INFO("Voice stopped\n");
   return 0;
}

/* The voice_is_playing method should only be called on non-streaming sources,
   and should return true if the voice is playing */
static bool _xaudio2_voice_is_playing(const ALLEGRO_VOICE *voice)
{
   ALLEGRO_ASSERT(!voice->is_streaming);
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;

   if (!ex_data) {
      ALLEGRO_WARN("ex_data is null\n");
      return false;
   }

   return !ex_data->stop_voice;
}

static unsigned int _xaudio2_get_voice_position(const ALLEGRO_VOICE *voice)
{
   ALLEGRO_ASSERT(!voice->is_streaming);
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;
   XAUDIO2_VOICE_STATE state;

   ex_data->voice->GetState(&state);

   return state.SamplesPlayed;
}

/* The set_voice_position method should set the voice's playback position,
   given the value in samples. This should never be called on a streaming
   voice. */
static int _xaudio2_set_voice_position(ALLEGRO_VOICE *voice, unsigned int val)
{
   ALLEGRO_ASSERT(!voice->is_streaming);
   ALLEGRO_XAUDIO2_DATA *ex_data = (ALLEGRO_XAUDIO2_DATA *)voice->extra;

//   ALLEGRO_ASSERT(ex_data->buffer.Flags & (XAUDIO2_END_OF_STREAM | XAUDIO2_LOOP_INFINITE));
   ex_data->buffer.PlayBegin = val; // TODO: submit!!!
   REPORT_FAILED_1( ex_data->voice->Start() );

   return 0;
}

extern "C"
ALLEGRO_AUDIO_DRIVER _al_kcm_xaudio2_driver = {
   "XAudio2",

   _xaudio2_open,
   _xaudio2_close,

   _xaudio2_allocate_voice,
   _xaudio2_deallocate_voice,

   _xaudio2_load_voice,
   _xaudio2_unload_voice,

   _xaudio2_start_voice,
   _xaudio2_stop_voice,

   _xaudio2_voice_is_playing,

   _xaudio2_get_voice_position,
   _xaudio2_set_voice_position,

   /* XAudio2 is playback-only */
   NULL, /* allocate_recorder */
   NULL, /* deallocate_recorder */
   // TODO: IMMDeviceEnumerator::EnumAudioEndpoints -> MMDeviceCollection
   NULL /* get_output_devices */
};

