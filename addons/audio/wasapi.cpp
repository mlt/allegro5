#define WIN32_LEAN_AND_MEAN
#define NOMCX
#define NOIME
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <stdint.h>
#include <stdio.h> // for snprintf
#include <string.h>

using Microsoft::WRL::ComPtr;

#define ALLEGRO_INTERNAL_UNSTABLE
#include "allegro5/allegro.h"

ALLEGRO_DEBUG_CHANNEL("WASAPI")

extern "C" {
#include "allegro5/internal/aintern_audio.h"
#include "allegro5/internal/aintern_wunicode.h"
}

#define BUFFER_READY 0
#define WANT_QUIT 1
#define BUFFER_READY_EVENT events[BUFFER_READY]
#define WANT_QUIT_EVENT events[WANT_QUIT]

/* Per-voice data for playback */
struct ALLEGRO_WASAPI_VOICE {
   int bits_per_sample;
   int channels;
   ComPtr<IAudioClient> audio_client;
   ComPtr<IAudioRenderClient> render_client;
   WAVEFORMATEX *mix_format; /* mix format returned by GetMixFormat or requested */
   UINT32 buffer_frames;
   HANDLE thread;
   HANDLE events[2]; /* buffer ready, want quit */
};

/* Per-recorder data for capture */
struct ALLEGRO_WASAPI_REC {
   ComPtr<IAudioClient> audio_client;
   ComPtr<IAudioCaptureClient> capture_client;
   WAVEFORMATEX *wfx;
   UINT32 buffer_frames;
   ALLEGRO_THREAD *thread;
   volatile bool should_quit;
};

/* Globals */
static ComPtr<IMMDeviceEnumerator> g_enum;
static _AL_LIST *g_output_device_list = NULL;
static int g_com_initialized = 0;
static char err_str[32];

#define REPORT_FAILED_1(expr) do { \
   HRESULT hr = expr; \
   if (FAILED(hr)) { \
      ALLEGRO_ERROR(#expr " failed: %s\n", get_error(hr)); \
      return 1; \
   } } while(0)

#define CASE_HR(x) case x: strcpy_s(err_str, sizeof(err_str), #x); break

static char *get_error(HRESULT hr)
{
   switch (hr) {
      CASE_HR(E_POINTER);
      CASE_HR(E_INVALIDARG);
      CASE_HR(E_OUTOFMEMORY);
      CASE_HR(E_NOTIMPL);
      CASE_HR(E_FAIL);
      CASE_HR(AUDCLNT_E_DEVICE_INVALIDATED);
      CASE_HR(AUDCLNT_E_DEVICE_IN_USE);
      CASE_HR(AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED);
      CASE_HR(AUDCLNT_E_EVENTHANDLE_NOT_SET);
      CASE_HR(AUDCLNT_E_NOT_INITIALIZED);
      CASE_HR(AUDCLNT_E_SERVICE_NOT_RUNNING);
      CASE_HR(AUDCLNT_E_UNSUPPORTED_FORMAT);
      default:
         snprintf(err_str, sizeof(err_str), "HRESULT 0x%08lX", (unsigned long)hr);
   }
   return err_str;
}

/* Playback thread (feeds audio to render client). Uses simple polling with Sleep(1). */
static DWORD WINAPI _wasapi_playback_thread(LPVOID param)
{
   ALLEGRO_VOICE *voice = (ALLEGRO_VOICE *)param;
   ALLEGRO_WASAPI_VOICE *ex = (ALLEGRO_WASAPI_VOICE *)voice->extra;
   if (!ex || !ex->audio_client || !ex->render_client || !ex->mix_format) return 1;

   const int channels = ex->mix_format->nChannels;
   const UINT32 buffer_frames = ex->buffer_frames;

   for (;;) {
      UINT32 padding = 0;

      switch (WaitForMultipleObjects(2, ex->events, FALSE, INFINITE) - WAIT_OBJECT_0) {
         case BUFFER_READY: break;
         case WANT_QUIT: return 0;
         default: ALLEGRO_ERROR("WaitForMultipleObjects failed in xaudio2 update thread\n"); return 0;
      }

      REPORT_FAILED_1( ex->audio_client->GetCurrentPadding(&padding) );
      UINT32 frames_available = buffer_frames - padding;
      ASSERT(frames_available);

      unsigned int frames = frames_available;

      /* Get audio buffer from render client */
      BYTE *pData = NULL;
      REPORT_FAILED_1( ex->render_client->GetBuffer(frames, &pData) );

      const void *data = _al_voice_update(voice, voice->mutex, &frames);
      if (data) {
         memcpy(pData, data, frames * sizeof(float) * channels);
      } else {
         al_fill_silence(pData, frames, voice->depth, voice->chan_conf);
      }

      REPORT_FAILED_1( ex->render_client->ReleaseBuffer(frames, 0) );
   }

   return 0;
}

/* Playback functions */

static int _wasapi_open(void)
{
   ALLEGRO_INFO("Starting WASAPI audio driver\n");
   if (!g_com_initialized) {
      HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("CoInitializeEx failed: %s\n", get_error(hr));
         return 1;
      }
      g_com_initialized = 1;
   }

   HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), (void**)&g_enum);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("CoCreateInstance(MMDeviceEnumerator) failed: %s\n", get_error(hr));
      return 1;
   }

   ALLEGRO_INFO("WASAPI opened\n");
   return 0;
}

static void _wasapi_close(void)
{
   ALLEGRO_DEBUG("Closing WASAPI driver\n");
   g_enum = nullptr;

   if (g_output_device_list) {
      _al_list_destroy(g_output_device_list);
      g_output_device_list = NULL;
   }

   if (g_com_initialized) {
      CoUninitialize();
      g_com_initialized = 0;
   }
   ALLEGRO_INFO("WASAPI closed\n");
}

/* allocate_voice: allocate per-voice structure */
static int _wasapi_allocate_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_DEBUG("Allocating voice\n");
   ALLEGRO_WASAPI_VOICE *ex = (ALLEGRO_WASAPI_VOICE *)al_calloc(1, sizeof(*ex));
   if (!ex) return 1;
   int bits = 0;
   switch (voice->depth & ~ALLEGRO_AUDIO_DEPTH_UNSIGNED) {
      case ALLEGRO_AUDIO_DEPTH_INT8: bits = 8; break;
      case ALLEGRO_AUDIO_DEPTH_INT16: bits = 16; break;
      case ALLEGRO_AUDIO_DEPTH_INT24: bits = 24; break;
      case ALLEGRO_AUDIO_DEPTH_FLOAT32: bits = 32; break;
      default:
         ALLEGRO_ERROR("WASAPI: unsupported audio depth\n");
         al_free(ex);
         return 1;
   }
   ex->bits_per_sample = bits;
   ex->channels = (int)al_get_channel_count(voice->chan_conf);
   ex->mix_format = NULL;
   ex->buffer_frames = 0;
   ex->thread = NULL;
   ex->BUFFER_READY_EVENT = CreateEvent(NULL, FALSE, FALSE, NULL);
   ex->WANT_QUIT_EVENT = CreateEvent(NULL, FALSE, FALSE, NULL);
   voice->extra = ex;
   ALLEGRO_DEBUG("Voice allocated\n");
   return 0;
}

static void _wasapi_deallocate_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_DEBUG("WASAPI: deallocate voice\n");
   ALLEGRO_WASAPI_VOICE *ex = (ALLEGRO_WASAPI_VOICE *)voice->extra;
   if (!ex) return;

   /* stop thread if running */
   SetEvent(ex->WANT_QUIT_EVENT);
   if (ex->thread) {
      WaitForSingleObject(ex->thread, INFINITE);
      CloseHandle(ex->thread);
      ex->thread = NULL;
      CloseHandle(ex->BUFFER_READY_EVENT);
      CloseHandle(ex->WANT_QUIT_EVENT);
   }

   ex->render_client = nullptr;
   ex->audio_client = nullptr;
   if (ex->mix_format) {
      CoTaskMemFree(ex->mix_format);
      ex->mix_format = NULL;
   }

   al_free(ex);
   voice->extra = NULL;
   ALLEGRO_DEBUG("WASAPI: voice deallocated\n");
}

/* Helper: get preferred device by friendly name from config (if present), else use default */
static HRESULT get_preferred_render_device(ComPtr<IMMDevice> *out_device)
{
   if (!g_enum) return E_FAIL;
   const char *cfg = al_get_config_value(al_get_system_config(), "wasapi", "device");
   ComPtr<IMMDeviceCollection> coll;
   HRESULT hr = g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
   if (FAILED(hr)) return hr;

   if (cfg && cfg[0] != '\0') {
      /* find device with matching friendly name (utf-8) */
      UINT count = 0;
      hr = coll->GetCount(&count);
      if (FAILED(hr)) return hr;
      for (UINT i = 0; i < count; ++i) {
         ComPtr<IMMDevice> dev;
         hr = coll->Item(i, &dev);
         if (FAILED(hr)) continue;
         ComPtr<IPropertyStore> props;
         hr = dev->OpenPropertyStore(STGM_READ, &props);
         if (FAILED(hr)) continue;
         PROPVARIANT var;
         PropVariantInit(&var);
         hr = props->GetValue(PKEY_Device_FriendlyName, &var);
         if (SUCCEEDED(hr) && var.vt == VT_LPWSTR) {
            char *name = _al_win_utf16_to_utf8(var.pwszVal);
            if (name) {
               bool match = strcmp(name, cfg) == 0;
               al_free(name);
               PropVariantClear(&var);
               if (match) {
                  *out_device = dev;
                  return S_OK;
               }
            }
         } else {
            PropVariantClear(&var);
         }
      }
      /* not found -> fall back to default */
   }

   /* default device */
   ComPtr<IMMDevice> dev;
   hr = g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
   if (FAILED(hr)) return hr;
   *out_device = dev;
   return S_OK;
}

/* start_voice: create audio client/render client + start thread for streaming voices.
   For non-streaming voices we also feed buffers until done. */
static int _wasapi_start_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_WASAPI_VOICE *ex = (ALLEGRO_WASAPI_VOICE *)voice->extra;
   if (!ex) return 1;

   HRESULT hr;
   ComPtr<IMMDevice> device;
   hr = get_preferred_render_device(&device);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("Get render device failed: %s\n", get_error(hr));
      return 1;
   }

   if (!ex->audio_client) {
      hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ex->audio_client);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("Activate IAudioClient failed: %s\n", get_error(hr));
         return 1;
      }

      WAVEFORMATEXTENSIBLE *mixFmt = NULL;
      hr = ex->audio_client->GetMixFormat((LPWAVEFORMATEX*)&mixFmt);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("GetMixFormat failed: %s\n", get_error(hr));
         return 1;
      }
      ALLEGRO_ASSERT(mixFmt->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
      ALLEGRO_ASSERT(mixFmt->Format.nChannels == ex->channels);
      ALLEGRO_ASSERT(mixFmt->Format.nSamplesPerSec == voice->frequency);
      ALLEGRO_ASSERT(mixFmt->Format.nBlockAlign == ex->channels * (ex->bits_per_sample >> 3));
      ALLEGRO_ASSERT(mixFmt->Format.nAvgBytesPerSec == mixFmt->Format.nBlockAlign * voice->frequency);
      ALLEGRO_ASSERT(mixFmt->Format.wBitsPerSample == ex->bits_per_sample);
      ALLEGRO_ASSERT(mixFmt->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
      ALLEGRO_ASSERT(mixFmt->Samples.wValidBitsPerSample == ex->bits_per_sample);

      /* store mix format for conversions */
      ex->mix_format = (WAVEFORMATEX*)mixFmt;

      /* Initialize audio client in shared mode using mix format */
      hr = ex->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, ex->mix_format, NULL);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("IAudioClient::Initialize failed: %s\n", get_error(hr));
         CoTaskMemFree(ex->mix_format);
         ex->mix_format = NULL;
         return 1;
      }

      UINT32 bufferFrames = 0;
      hr = ex->audio_client->GetBufferSize(&bufferFrames);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("GetBufferSize failed: %s\n", get_error(hr));
         return 1;
      }
      ex->buffer_frames = bufferFrames;

      ComPtr<IAudioRenderClient> renderClient;
      hr = ex->audio_client->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("GetService(IAudioRenderClient) failed: %s\n", get_error(hr));
         return 1;
      }
      ex->render_client = renderClient;
   }

   ex->BUFFER_READY_EVENT = CreateEvent(NULL, FALSE, FALSE, NULL);
   ex->WANT_QUIT_EVENT = CreateEvent(NULL, FALSE, FALSE, NULL);

   if (!(ex->BUFFER_READY_EVENT && ex->WANT_QUIT_EVENT)) {
      ALLEGRO_ERROR("CreateEvent failed for playback\n");
      return 1;
   }
   REPORT_FAILED_1( ex->audio_client->SetEventHandle(ex->BUFFER_READY_EVENT) );

   ex->thread = CreateThread(NULL, 0, _wasapi_playback_thread, voice, 0, NULL);
   if (!ex->thread) {
      ALLEGRO_ERROR("CreateThread failed for playback\n");
      return 1;
   }

   REPORT_FAILED_1( ex->audio_client->Start() );

   ALLEGRO_INFO("WASAPI: voice started\n");
   return 0;
}

static int _wasapi_stop_voice(ALLEGRO_VOICE *voice)
{
   ALLEGRO_WASAPI_VOICE *ex = (ALLEGRO_WASAPI_VOICE *)voice->extra;
   if (!ex) return 1;

   SetEvent(ex->WANT_QUIT_EVENT);
   if (ex->thread) {
      WaitForSingleObject(ex->thread, INFINITE);
      CloseHandle(ex->thread);
      ex->thread = NULL;
      CloseHandle(ex->BUFFER_READY_EVENT);
      CloseHandle(ex->WANT_QUIT_EVENT);
   }
   if (ex->audio_client) {
      ex->audio_client->Stop();
   }

   ALLEGRO_INFO("WASAPI: voice stopped\n");
   return 0;
}

static bool _wasapi_voice_is_playing(const ALLEGRO_VOICE *voice)
{
   ALLEGRO_WASAPI_VOICE *ex = (ALLEGRO_WASAPI_VOICE *)voice->extra;
   if (!ex) return false;

   return WaitForSingleObject(ex->WANT_QUIT_EVENT, 0) == WAIT_TIMEOUT;
}

static unsigned int _wasapi_get_voice_position(const ALLEGRO_VOICE *voice)
{
   /* Precise sample position reporting would require format-specific queries.
      Return 0 to keep it simple (consistent with other minimal drivers). */
   (void)voice;
   return 0;
}

static int _wasapi_set_voice_position(ALLEGRO_VOICE *voice, unsigned int val)
{
   (void)voice;
   (void)val;
   ALLEGRO_WARN("WASAPI: set_voice_position not implemented\n");
   return 1;
}

/* Recorder (capture) thread: runs under Allegro thread model.
   This thread will be created by allocate_recorder() via al_create_thread and
   will be started by al_create_audio_recorder (the caller). */
static void *wasapi_capture_thread(ALLEGRO_THREAD *self, void *arg)
{
   ALLEGRO_AUDIO_RECORDER *r = (ALLEGRO_AUDIO_RECORDER *)arg;
   if (!r) return NULL;
   ALLEGRO_WASAPI_REC *rec = (ALLEGRO_WASAPI_REC *)r->extra;
   if (!rec) return NULL;

   UINT32 packetFrames = 0;
   unsigned int frag_i = 0;

   ALLEGRO_DEBUG("WASAPI: recorder thread started\n");

   while (!al_get_thread_should_stop(self)) {
      /* If not recording, still read/discard some data to avoid bursts later */
      if (!r->is_recording) {
         /* Poll capture client for data */
         rec->capture_client->GetNextPacketSize(&packetFrames);
         if (packetFrames == 0) {
            al_rest(0.005);
            continue;
         }
         BYTE *data = NULL;
         UINT32 framesAvailable = 0;
         DWORD flags = 0;
         HRESULT hr = rec->capture_client->GetBuffer(&data, &framesAvailable, &flags, NULL, NULL);
         if (SUCCEEDED(hr) && framesAvailable > 0) {
            /* discard into temporary buffer (do not use fragments since not recording) */
            rec->capture_client->ReleaseBuffer(framesAvailable);
         }
         else {
            al_rest(0.002);
         }
         continue;
      }

      /* recording active */
      rec->capture_client->GetNextPacketSize(&packetFrames);
      if (packetFrames == 0) {
         al_rest(0.002);
         continue;
      }

      BYTE *data = NULL;
      UINT32 framesAvailable = 0;
      DWORD flags = 0;
      HRESULT hr = rec->capture_client->GetBuffer(&data, &framesAvailable, &flags, NULL, NULL);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("IAudioCaptureClient::GetBuffer failed: %s\n", get_error(hr));
         al_rest(0.005);
         continue;
      }
      if (framesAvailable == 0) {
         rec->capture_client->ReleaseBuffer(framesAvailable);
         continue;
      }

      /* Copy into fragment buffer and emit event */
      size_t bytes = framesAvailable * r->sample_size;
      if (bytes > r->fragment_size) {
         /* oversized frame -> clamp */
         bytes = r->fragment_size;
         framesAvailable = (UINT32)(bytes / r->sample_size);
      }
      memcpy(r->fragments[frag_i], data, bytes);

      ALLEGRO_EVENT user_event;
      user_event.user.type = ALLEGRO_EVENT_AUDIO_RECORDER_FRAGMENT;
      ALLEGRO_AUDIO_RECORDER_EVENT *ev = al_get_audio_recorder_event(&user_event);
      ev->buffer = r->fragments[frag_i];
      ev->samples = framesAvailable;
      al_emit_user_event(&r->source, &user_event, NULL);

      if (++frag_i == r->fragment_count) frag_i = 0;

      rec->capture_client->ReleaseBuffer(framesAvailable);
   }

   ALLEGRO_DEBUG("WASAPI: recorder thread exiting\n");
   return NULL;
}

/* allocate_recorder: create capture client and an Allegro thread (not started here) */
static int _wasapi_allocate_recorder(ALLEGRO_AUDIO_RECORDER *r)
{
   if (!g_enum) return 1;

   ALLEGRO_WASAPI_REC *rec = (ALLEGRO_WASAPI_REC *)al_calloc(1, sizeof(*rec));
   if (!rec) return 1;

   HRESULT hr;
   ComPtr<IMMDevice> device;
   /* Allow config to select capture device by friendly name */
   const char *cfg = al_get_config_value(al_get_system_config(), "wasapi", "capture_device");
   if (cfg && cfg[0] != '\0') {
      /* try to find by friendly name */
      ComPtr<IMMDeviceCollection> coll;
      hr = g_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
      if (SUCCEEDED(hr)) {
         UINT count = 0;
         coll->GetCount(&count);
         for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> d;
            if (SUCCEEDED(coll->Item(i, &d))) {
               ComPtr<IPropertyStore> props;
               if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                  PROPVARIANT var;
                  PropVariantInit(&var);
                  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
                     char *name = _al_win_utf16_to_utf8(var.pwszVal);
                     if (name && strcmp(name, cfg) == 0) {
                        device = d;
                        CoTaskMemFree(var.pwszVal);
                        PropVariantClear(&var);
                        if (name) al_free(name);
                        break;
                     }
                     if (name) al_free(name);
                  }
                  PropVariantClear(&var);
               }
            }
         }
      }
   }

   if (!device) {
      hr = g_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
      if (FAILED(hr)) {
         ALLEGRO_ERROR("GetDefaultAudioEndpoint (capture) failed: %s\n", get_error(hr));
         al_free(rec);
         return 1;
      }
   }

   hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&rec->audio_client);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("Activate IAudioClient (capture) failed: %s\n", get_error(hr));
      al_free(rec);
      return 1;
   }

   /* Try to use requested format if possible */
   WAVEFORMATEX want = {};
   want.wFormatTag = WAVE_FORMAT_PCM;
   want.nChannels = (WORD)al_get_channel_count(r->chan_conf);
   want.nSamplesPerSec = r->frequency;
   want.wBitsPerSample = (WORD)(al_get_audio_depth_size(r->depth) * 8);
   want.nBlockAlign = (WORD)((want.nChannels * want.wBitsPerSample) / 8);
   want.nAvgBytesPerSec = want.nSamplesPerSec * want.nBlockAlign;

   WAVEFORMATEX *closest = NULL;
   hr = rec->audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &want, &closest);
   if (hr == S_OK) {
      /* requested format supported */
      rec->wfx = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
      memcpy(rec->wfx, &want, sizeof(WAVEFORMATEX));
   } else if (hr == S_FALSE && closest) {
      /* system returned a closest format we must use; store it */
      rec->wfx = closest; /* already allocated by the API via CoTaskMemAlloc */
   } else {
      /* last resort: get system mix format */
      WAVEFORMATEX *mix = NULL;
      if (SUCCEEDED(rec->audio_client->GetMixFormat(&mix))) {
         rec->wfx = mix;
      } else {
         ALLEGRO_ERROR("No compatible capture format\n");
         al_free(rec);
         return 1;
      }
   }

   /* Init the audio client */
   hr = rec->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, rec->wfx, NULL);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("IAudioClient::Initialize (capture) failed: %s\n", get_error(hr));
      if (rec->wfx) CoTaskMemFree(rec->wfx);
      al_free(rec);
      return 1;
   }

   /* Get buffer size and capture client */
   hr = rec->audio_client->GetBufferSize(&rec->buffer_frames);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("GetBufferSize (capture) failed: %s\n", get_error(hr));
      if (rec->wfx) CoTaskMemFree(rec->wfx);
      al_free(rec);
      return 1;
   }

   hr = rec->audio_client->GetService(__uuidof(IAudioCaptureClient), (void**)&rec->capture_client);
   if (FAILED(hr)) {
      ALLEGRO_ERROR("GetService(IAudioCaptureClient) failed: %s\n", get_error(hr));
      if (rec->wfx) CoTaskMemFree(rec->wfx);
      al_free(rec);
      return 1;
   }

   /* Prepare Allegro thread object (do not start here) */
   rec->thread = al_create_thread(wasapi_capture_thread, r);
   rec->should_quit = false;
   r->extra = rec;

   /* Note: we don't start the thread here. al_create_audio_recorder will create
      r->mutex/r->cond and then call al_start_thread if r->thread != NULL. */

   ALLEGRO_DEBUG("WASAPI: recorder allocated\n");
   return 0;
}

static void _wasapi_deallocate_recorder(ALLEGRO_AUDIO_RECORDER *r)
{
   if (!r || !r->extra) return;
   ALLEGRO_WASAPI_REC *rec = (ALLEGRO_WASAPI_REC *)r->extra;

   /* Stop thread if running */
   if (rec->thread) {
      al_set_thread_should_stop(rec->thread);
      al_join_thread(rec->thread, NULL);
      al_destroy_thread(rec->thread);
      rec->thread = NULL;
   }

   if (rec->audio_client) {
      rec->audio_client->Stop();
   }
   rec->capture_client = nullptr;
   rec->audio_client = nullptr;
   if (rec->wfx) {
      CoTaskMemFree(rec->wfx);
      rec->wfx = NULL;
   }

   al_free(rec);
   r->extra = NULL;
}

/* Device enumeration: returns a lazily built _AL_LIST of ALLEGRO_AUDIO_DEVICE */
static void _output_device_list_dtor(void *value, void *userdata)
{
   (void)userdata;
   ALLEGRO_AUDIO_DEVICE *d = (ALLEGRO_AUDIO_DEVICE *)value;
   if (!d) return;
   al_free(d->name);
   al_free(d->identifier);
   al_free(d);
}

static _AL_LIST* _wasapi_get_output_devices(void)
{
   if (g_output_device_list) return g_output_device_list;
   if (!g_enum) return NULL;

   g_output_device_list = _al_list_create();

   ComPtr<IMMDeviceCollection> coll;
   HRESULT hr = g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
   if (FAILED(hr)) return g_output_device_list;

   UINT count = 0;
   coll->GetCount(&count);
   for (UINT i = 0; i < count; ++i) {
      ComPtr<IMMDevice> dev;
      if (FAILED(coll->Item(i, &dev))) continue;
      LPWSTR id = NULL;
      if (FAILED(dev->GetId(&id)) || !id) continue;

      ComPtr<IPropertyStore> props;
      if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) {
         CoTaskMemFree(id);
         continue;
      }
      PROPVARIANT var;
      PropVariantInit(&var);
      if (FAILED(props->GetValue(PKEY_Device_FriendlyName, &var)) || var.vt != VT_LPWSTR) {
         PropVariantClear(&var);
         CoTaskMemFree(id);
         continue;
      }

      char *name = _al_win_utf16_to_utf8(var.pwszVal);
      char *identifier = _al_win_utf16_to_utf8(id);

      /* Build ALLEGRO_AUDIO_DEVICE */
      ALLEGRO_AUDIO_DEVICE *adev = (ALLEGRO_AUDIO_DEVICE *)al_malloc(sizeof(ALLEGRO_AUDIO_DEVICE));
      adev->name = name ? name : strdup("Unknown");
      adev->identifier = identifier ? identifier : strdup("");

      _al_list_push_back_ex(g_output_device_list, adev, _output_device_list_dtor);

      PropVariantClear(&var);
      CoTaskMemFree(id);
   }

   return g_output_device_list;
}

int _wasapi_probe_format(unsigned int* frequency, ALLEGRO_AUDIO_DEPTH* depth, ALLEGRO_CHANNEL_CONF* chan_conf)
{
   ComPtr<IMMDevice> device;
   ComPtr<IAudioClient> audio_client;
   WAVEFORMATEXTENSIBLE* mixFmt = NULL;
   int ret = 0;

   REPORT_FAILED_1( get_preferred_render_device(&device) );
   REPORT_FAILED_1( device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audio_client) );
   REPORT_FAILED_1( audio_client->GetMixFormat((LPWAVEFORMATEX*)&mixFmt) );

   int channels = (int)al_get_channel_count(*chan_conf);
   int bytes_per_sample = (int)al_get_audio_depth_size(*depth);
   int bits_per_sample = bytes_per_sample << 3;

   ALLEGRO_ASSERT(mixFmt->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
   ALLEGRO_ASSERT(mixFmt->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
   if (mixFmt->Format.nChannels != channels) {
      *chan_conf = _al_count_to_channel_conf(channels = mixFmt->Format.nChannels);
      ret = 1;
   }
   if (mixFmt->Format.nSamplesPerSec != *frequency) {
      *frequency = mixFmt->Format.nSamplesPerSec;
      ret = 1;
   }
   if (mixFmt->Format.wBitsPerSample != bits_per_sample) {
      bytes_per_sample = mixFmt->Format.wBitsPerSample >> 3;
      *depth = _al_word_size_to_depth_conf(bytes_per_sample);
      ret = 1;
   }
   ALLEGRO_ASSERT(mixFmt->Format.nBlockAlign == channels * bytes_per_sample);
   ALLEGRO_ASSERT(mixFmt->Format.nAvgBytesPerSec == channels * bytes_per_sample * *frequency);

   return ret;
}

/* Export driver */
extern "C"
ALLEGRO_AUDIO_DRIVER _al_kcm_wasapi_driver = {
   "WASAPI",
   _wasapi_open,
   _wasapi_close,
   _wasapi_allocate_voice,
   _wasapi_deallocate_voice,
   /* load/unload voice are no-ops here because we create clients in start */
   NULL,
   NULL,
   _wasapi_start_voice,
   _wasapi_stop_voice,
   _wasapi_voice_is_playing,
   _wasapi_get_voice_position,
   _wasapi_set_voice_position,
   _wasapi_allocate_recorder,
   _wasapi_deallocate_recorder,
   _wasapi_get_output_devices,
   _wasapi_probe_format
};
