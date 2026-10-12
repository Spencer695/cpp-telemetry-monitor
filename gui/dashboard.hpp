#pragma once
#include "telemetry/server.hpp"
#include <QMainWindow>
#include <QObject>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QThread;
class QTimer;
class QTcpSocket;

struct DashboardState {
    static constexpr std::size_t max_devices = 128;
    std::mutex mutex;
    std::uint16_t port = 0;
    telemetry::Status status;
    std::map<std::string, telemetry::Reading> latest;
    bool display_limited = false;
    telemetry::ServerObserver observer;
};

class ServerWorker final : public QObject {
    Q_OBJECT
public:
    ServerWorker(telemetry::Config config, std::shared_ptr<DashboardState> state);
public slots:
    void run();
signals:
    void completed(int code, const QString& error);
private:
    telemetry::Config config_;
    std::shared_ptr<DashboardState> state_;
};

class Dashboard final : public QMainWindow {
    Q_OBJECT
public:
    explicit Dashboard(QWidget* parent = nullptr);
    ~Dashboard() override;
    bool running() const { return thread_ != nullptr; }
    std::uint16_t listeningPort() const;
    std::uint64_t processed() const;
    std::size_t deviceCount() const;
    double latestValue(const std::string& device) const;
signals:
    void serverCompleted(int code);
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void startServer();
    void stopServer();
    void refresh();
    void startDemo();
    void stopDemo();
    void updateControls();
    QSpinBox *port_, *capacity_;
    QLineEdit* log_;
    QPushButton *start_, *stop_, *demo_, *browse_;
    QLabel *state_label_, *address_, *connected_, *processed_, *dropped_, *malformed_, *detail_;
    QTableWidget* table_;
    QTimer *refresh_timer_, *demo_timer_;
    QTcpSocket* demo_socket_;
    QThread* thread_ = nullptr;
    std::shared_ptr<DashboardState> state_;
    bool stopping_ = false, close_requested_ = false;
    unsigned demo_sequence_ = 0;
};
