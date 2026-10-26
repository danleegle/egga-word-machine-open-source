#ifndef CLIENT_MGMT_SERVICE_H
#define CLIENT_MGMT_SERVICE_H

class ClientMgmtService {
public:
    static ClientMgmtService& GetInstance();

    void Start();
    void ImportWifiConfigFromAssetsOnce();

private:
    ClientMgmtService() = default;

    bool started_ = false;
    bool imported_wifi_config_ = false;
};

#endif  // CLIENT_MGMT_SERVICE_H
