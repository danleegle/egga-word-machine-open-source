#include "online_pronunciation_service.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "wifi_manager.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr char kTag[] = "OnlinePronunciation";
constexpr char kYoudaoBaseUrl[] = "http://dict.youdao.com/dictvoice";
constexpr int kHttpTimeoutMs = 2500;
constexpr size_t kHttpReadBufferBytes = 1024;
constexpr size_t kMaxOnlineAudioBytes = 256 * 1024;
constexpr size_t kInitialPcmBufferBytes = 8192;

std::mutex g_decoder_mutex;
std::mutex g_cache_mutex;
struct CachedAudio {
    std::string key;
    std::vector<uint8_t> audio;
};
std::vector<CachedAudio> g_cached_audio;
constexpr size_t kMaxCachedAudioEntries = 4;
constexpr size_t kMaxCachedAudioBytes = 512 * 1024;

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool ShouldContinue(const std::function<bool()>& should_continue) {
    return !should_continue || should_continue();
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char ch : value) {
        const bool unreserved =
            std::isalnum(ch) != 0 ||
            ch == '-' ||
            ch == '_' ||
            ch == '.' ||
            ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4) & 0x0f]);
        encoded.push_back(kHex[ch & 0x0f]);
    }
    return encoded;
}

bool DownloadAudioUrl(const std::string& url,
                      const std::function<bool()>& should_continue,
                      std::vector<uint8_t>* out,
                      std::string* error,
                      const char* referer = nullptr) {
    if (!WifiManager::GetInstance().IsConnected()) {
        SetError(error, "wifi is not connected");
        return false;
    }
    if (!ShouldContinue(should_continue)) {
        SetError(error, "request superseded");
        return false;
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        SetError(error, "unsupported pronunciation URL");
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = kHttpReadBufferBytes;
    config.buffer_size_tx = 512;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        SetError(error, "failed to initialize HTTP client");
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "Egga/1.0");
    if (referer != nullptr && referer[0] != '\0') {
        esp_http_client_set_header(client, "Referer", referer);
    }

    bool ok = false;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        SetError(error, std::string("HTTP open failed: ") + esp_err_to_name(err));
    } else {
        const int64_t content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            SetError(error, "HTTP status " + std::to_string(status));
        } else if (content_length > static_cast<int64_t>(kMaxOnlineAudioBytes)) {
            SetError(error, "online audio exceeds size limit");
        } else {
            out->clear();
            if (content_length > 0) {
                out->reserve(static_cast<size_t>(content_length));
            }

            std::vector<char> buffer(kHttpReadBufferBytes);
            while (ShouldContinue(should_continue)) {
                const int read = esp_http_client_read(client, buffer.data(), buffer.size());
                if (read < 0) {
                    SetError(error, "HTTP read failed");
                    break;
                }
                if (read == 0) {
                    ok = !out->empty();
                    if (!ok) {
                        SetError(error, "empty HTTP response");
                    }
                    break;
                }
                if (out->size() + static_cast<size_t>(read) > kMaxOnlineAudioBytes) {
                    SetError(error, "online audio exceeds size limit");
                    break;
                }
                out->insert(out->end(),
                            reinterpret_cast<uint8_t*>(buffer.data()),
                            reinterpret_cast<uint8_t*>(buffer.data()) + read);
            }
            if (!ShouldContinue(should_continue)) {
                SetError(error, "request superseded");
                ok = false;
            }
        }
        esp_http_client_close(client);
    }

    esp_http_client_cleanup(client);
    return ok;
}

std::string BuildYoudaoUrl(const std::string& word, int voice_type) {
    const int normalized_voice_type = voice_type == 1 ? 1 : 0;
    return std::string(kYoudaoBaseUrl) +
        "?type=" + std::to_string(normalized_voice_type) +
        "&audio=" + UrlEncode(word);
}

std::string DirectCacheKey(const std::string& url) {
    return "url:" + url;
}

std::string WordCacheKey(const std::string& word, int voice_type) {
    const int normalized_voice_type = voice_type == 1 ? 1 : 0;
    return "youdao:" + std::to_string(normalized_voice_type) + ":" + word;
}

bool ReadCachedAudio(const std::string& key, std::vector<uint8_t>* out) {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    const auto it = std::find_if(g_cached_audio.begin(), g_cached_audio.end(),
                                 [&key](const CachedAudio& entry) {
                                     return entry.key == key && !entry.audio.empty();
                                 });
    if (key.empty() || it == g_cached_audio.end()) {
        return false;
    }
    CachedAudio entry = std::move(*it);
    g_cached_audio.erase(it);
    g_cached_audio.push_back(std::move(entry));
    *out = g_cached_audio.back().audio;
    return true;
}

void StoreCachedAudio(const std::string& key, const std::vector<uint8_t>& audio) {
    if (key.empty() || audio.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cached_audio.erase(
        std::remove_if(g_cached_audio.begin(), g_cached_audio.end(),
                       [&key](const CachedAudio& entry) { return entry.key == key; }),
        g_cached_audio.end());
    g_cached_audio.push_back(CachedAudio{key, audio});

    auto cache_bytes = []() {
        size_t total = 0;
        for (const CachedAudio& entry : g_cached_audio) {
            total += entry.audio.size();
        }
        return total;
    };
    while (g_cached_audio.size() > 1 &&
           (g_cached_audio.size() > kMaxCachedAudioEntries ||
            cache_bytes() > kMaxCachedAudioBytes)) {
        g_cached_audio.erase(g_cached_audio.begin());
    }
}

bool DownloadAndCache(const std::string& url,
                      const std::string& cache_key,
                      const std::function<bool()>& should_continue,
                      std::string* error,
                      const char* referer = nullptr) {
    std::vector<uint8_t> audio;
    if (!DownloadAudioUrl(url, should_continue, &audio, error, referer)) {
        return false;
    }
    if (!ShouldContinue(should_continue)) {
        SetError(error, "request superseded");
        return false;
    }
    StoreCachedAudio(cache_key, audio);
    return true;
}

bool ConvertPcmToOutput(const uint8_t* data,
                        size_t bytes,
                        const esp_audio_simple_dec_info_t& info,
                        int output_rate,
                        std::vector<int16_t>* pcm) {
    if (data == nullptr || pcm == nullptr ||
        info.bits_per_sample != 16 ||
        info.channel == 0 ||
        info.channel > 2 ||
        info.sample_rate == 0) {
        return false;
    }

    const size_t input_frame_bytes = static_cast<size_t>(info.channel) * sizeof(int16_t);
    const size_t input_frames = bytes / input_frame_bytes;
    if (input_frames == 0) {
        pcm->clear();
        return true;
    }

    const size_t output_frames = std::max<size_t>(
        1,
        input_frames * static_cast<size_t>(output_rate) / static_cast<size_t>(info.sample_rate));
    pcm->resize(output_frames);

    for (size_t i = 0; i < output_frames; ++i) {
        const size_t src_frame = std::min(
            input_frames - 1,
            i * static_cast<size_t>(info.sample_rate) / static_cast<size_t>(output_rate));
        const uint8_t* frame = data + src_frame * input_frame_bytes;
        const int16_t left = static_cast<int16_t>(frame[0] | (frame[1] << 8));
        if (info.channel == 1) {
            (*pcm)[i] = left;
        } else {
            const int16_t right = static_cast<int16_t>(frame[2] | (frame[3] << 8));
            (*pcm)[i] = static_cast<int16_t>((static_cast<int>(left) + static_cast<int>(right)) / 2);
        }
    }
    return true;
}

bool DecodeAndPlayMp3(const std::vector<uint8_t>& mp3,
                      const std::function<bool()>& should_continue,
                      std::string* error) {
    if (mp3.empty()) {
        SetError(error, "empty MP3 data");
        return false;
    }

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        SetError(error, "audio codec unavailable");
        return false;
    }

    std::lock_guard<std::mutex> decoder_lock(g_decoder_mutex);
    if (!ShouldContinue(should_continue)) {
        SetError(error, "request superseded");
        return false;
    }

    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
    };

    esp_audio_err_t ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        SetError(error, "MP3 decoder open failed: " + std::to_string(ret));
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        return false;
    }

    std::vector<uint8_t> out_buffer(kInitialPcmBufferBytes);
    std::vector<int16_t> pcm;
    esp_audio_simple_dec_info_t info = {};
    bool have_info = false;
    bool played_any = false;
    bool success = true;

    Application::GetInstance().StopSound();
    const bool output_was_enabled = codec->output_enabled();
    if (!output_was_enabled) {
        codec->EnableOutput(true);
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer = const_cast<uint8_t*>(mp3.data()),
        .len = static_cast<uint32_t>(mp3.size()),
        .eos = true,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };

    while (raw.len > 0 && ShouldContinue(should_continue)) {
        esp_audio_simple_dec_out_t out_frame = {
            .buffer = out_buffer.data(),
            .len = static_cast<uint32_t>(out_buffer.size()),
            .needed_size = 0,
            .decoded_size = 0,
        };

        ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out_frame.needed_size == 0) {
                SetError(error, "MP3 decoder requested empty output buffer");
                success = false;
                break;
            }
            out_buffer.resize(out_frame.needed_size);
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            SetError(error, "MP3 decode failed: " + std::to_string(ret));
            success = played_any;
            break;
        }

        if (out_frame.decoded_size > 0) {
            if (!have_info) {
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                    SetError(error, "MP3 decoder info unavailable");
                    success = false;
                    break;
                }
                have_info = true;
                ESP_LOGI(kTag, "Youdao audio info: sample_rate=%u bits=%u channel=%u",
                         static_cast<unsigned>(info.sample_rate),
                         static_cast<unsigned>(info.bits_per_sample),
                         static_cast<unsigned>(info.channel));
            }

            const int output_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
            if (!ConvertPcmToOutput(out_frame.buffer, out_frame.decoded_size, info, output_rate, &pcm)) {
                SetError(error, "unsupported MP3 PCM format");
                success = false;
                break;
            }
            if (!pcm.empty()) {
                codec->OutputData(pcm);
                played_any = true;
            }
        }

        if (raw.consumed == 0) {
            if (out_frame.decoded_size == 0) {
                SetError(error, "MP3 decoder made no progress");
                success = false;
            }
            break;
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }

    if (!ShouldContinue(should_continue)) {
        SetError(error, "request superseded");
        success = false;
    }

    if (!output_was_enabled) {
        codec->EnableOutput(false);
    }

    esp_audio_simple_dec_close(decoder);
    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();
    return success && played_any;
}

}  // namespace

bool OnlinePronunciationService::PrefetchUrl(const std::string& url,
                                             const std::function<bool()>& should_continue,
                                             std::string* error) {
    const std::string cache_key = DirectCacheKey(url);
    std::vector<uint8_t> cached;
    if (ReadCachedAudio(cache_key, &cached)) {
        return true;
    }
    return DownloadAndCache(url,
                            cache_key,
                            should_continue,
                            error,
                            "https://tc-apis.maimemo.com/webstudy/app");
}

bool OnlinePronunciationService::PrefetchWord(const std::string& word,
                                              int voice_type,
                                              const std::function<bool()>& should_continue,
                                              std::string* error) {
    const std::string cache_key = WordCacheKey(word, voice_type);
    std::vector<uint8_t> cached;
    if (ReadCachedAudio(cache_key, &cached)) {
        return true;
    }
    return DownloadAndCache(BuildYoudaoUrl(word, voice_type),
                            cache_key,
                            should_continue,
                            error);
}

bool OnlinePronunciationService::FetchUrlAndPlay(const std::string& url,
                                                 const std::function<bool()>& should_continue,
                                                 std::string* error) {
    const std::string cache_key = DirectCacheKey(url);
    std::vector<uint8_t> mp3;
    if (!ReadCachedAudio(cache_key, &mp3)) {
        if (!DownloadAudioUrl(url,
                              should_continue,
                              &mp3,
                              error,
                              "https://tc-apis.maimemo.com/webstudy/app")) {
            return false;
        }
        StoreCachedAudio(cache_key, mp3);
    }
    if (!ShouldContinue(should_continue)) {
        SetError(error, "request superseded");
        return false;
    }
    ESP_LOGI(kTag, "Downloaded direct pronunciation: %u bytes",
             static_cast<unsigned>(mp3.size()));
    return DecodeAndPlayMp3(mp3, should_continue, error);
}

bool OnlinePronunciationService::FetchAndPlay(const std::string& word,
                                              int voice_type,
                                              const std::function<bool()>& should_continue,
                                              std::string* error) {
    const std::string cache_key = WordCacheKey(word, voice_type);
    std::vector<uint8_t> mp3;
    if (!ReadCachedAudio(cache_key, &mp3)) {
        if (!DownloadAudioUrl(BuildYoudaoUrl(word, voice_type), should_continue, &mp3, error)) {
            return false;
        }
        StoreCachedAudio(cache_key, mp3);
    }
    if (!ShouldContinue(should_continue)) {
        SetError(error, "request superseded");
        return false;
    }
    ESP_LOGI(kTag, "Downloaded Youdao pronunciation for %s: %u bytes",
             word.c_str(),
             static_cast<unsigned>(mp3.size()));
    return DecodeAndPlayMp3(mp3, should_continue, error);
}
