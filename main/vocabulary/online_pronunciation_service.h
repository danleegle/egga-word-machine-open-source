#ifndef ONLINE_PRONUNCIATION_SERVICE_H
#define ONLINE_PRONUNCIATION_SERVICE_H

#include <functional>
#include <string>

class OnlinePronunciationService {
public:
    static bool PrefetchUrl(const std::string& url,
                            const std::function<bool()>& should_continue,
                            std::string* error);
    static bool PrefetchWord(const std::string& word,
                             int voice_type,
                             const std::function<bool()>& should_continue,
                             std::string* error);
    static bool FetchUrlAndPlay(const std::string& url,
                                const std::function<bool()>& should_continue,
                                std::string* error);
    static bool FetchAndPlay(const std::string& word,
                             int voice_type,
                             const std::function<bool()>& should_continue,
                             std::string* error);
};

#endif  // ONLINE_PRONUNCIATION_SERVICE_H
