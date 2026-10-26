#ifndef MAIMEMO_OPEN_API_CLIENT_H
#define MAIMEMO_OPEN_API_CLIENT_H

#include "maimemo_offline_store.h"

#include <string>
#include <vector>

class MaimemoOpenApiClient {
public:
    bool FetchTodayItems(const std::string& token,
                         std::vector<MaimemoTodayItem>* items,
                         std::string* error) const;
};

#endif  // MAIMEMO_OPEN_API_CLIENT_H
