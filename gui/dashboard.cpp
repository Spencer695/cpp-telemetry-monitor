#include "dashboard.hpp"
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

ServerWorker::ServerWorker(telemetry::Config config, std::shared_ptr<DashboardState> state)
    : config_(std::move(config)), state_(std::move(state)) {
    const auto shared = state_;
    state_->observer.listening = [shared](std::uint16_t port) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->port = port;
    };
    state_->observer.reading = [shared](const telemetry::Reading& reading) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->latest.count(reading.device) || shared->latest.size() < DashboardState::max_devices)
            shared->latest.insert_or_assign(reading.device, reading);
        else shared->display_limited = true;
    };
    state_->observer.status = [shared](const telemetry::Status& status) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->status = status;
    };
}
void ServerWorker::run() {
    int code = 0;
    QString error;
    try { code = telemetry::run_server(config_, &state_->observer); }
    catch (const std::exception& failure) { code = 1; error = QString::fromUtf8(failure.what()); }
    // Break callback/shared-state ownership cycles after the backend has joined its threads.
    state_->observer.listening = {};
    state_->observer.reading = {};
    state_->observer.status = {};
    emit completed(code, error);
}

Dashboard::Dashboard(QWidget* parent) : QMainWindow(parent), state_(std::make_shared<DashboardState>()) {
    setWindowTitle("Telemetry Monitor");
    resize(1080, 730);
    setMinimumSize(820, 620);
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(18);
    auto* header = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    auto* title = new QLabel("Telemetry Monitor"); title->setObjectName("title");
    titles->addWidget(title);
    titles->addWidget(new QLabel("Live readings from local simulated devices"));
    header->addLayout(titles);
    header->addStretch();
    state_label_ = new QLabel("Stopped"); state_label_->setObjectName("state");
    header->addWidget(state_label_);
    layout->addLayout(header);

    auto* metrics = new QHBoxLayout;
    auto card = [&](const QString& caption, QLabel*& value, const QString& name) {
        auto* frame = new QFrame; frame->setObjectName("card");
        auto* inner = new QVBoxLayout(frame);
        inner->setContentsMargins(18, 14, 18, 14);
        inner->addWidget(new QLabel(caption));
        value = new QLabel("0"); value->setObjectName(name); value->setProperty("metric", true);
        inner->addWidget(value);
        metrics->addWidget(frame);
    };
    card("CONNECTED", connected_, "connected");
    card("PROCESSED", processed_, "processed");
    card("DROPPED", dropped_, "dropped");
    card("INVALID", malformed_, "malformed");
    layout->addLayout(metrics);

    auto* settings = new QHBoxLayout;
    auto* port_box = new QVBoxLayout;
    port_box->addWidget(new QLabel("TCP port (0 = automatic)"));
    port_ = new QSpinBox; port_->setObjectName("port"); port_->setRange(0, 65535); port_->setValue(9000);
    port_box->addWidget(port_); settings->addLayout(port_box);
    auto* capacity_box = new QVBoxLayout;
    capacity_box->addWidget(new QLabel("Queue capacity"));
    capacity_ = new QSpinBox; capacity_->setObjectName("capacity"); capacity_->setRange(1, 100000); capacity_->setValue(256);
    capacity_box->addWidget(capacity_); settings->addLayout(capacity_box);
    auto* log_box = new QVBoxLayout;
    log_box->addWidget(new QLabel("Capture file"));
    auto* file_row = new QHBoxLayout;
    log_ = new QLineEdit(QDir::current().filePath("capture-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss-zzz") + ".csv"));
    log_->setObjectName("logPath");
    browse_ = new QPushButton("Browse");
    file_row->addWidget(log_); file_row->addWidget(browse_);
    log_box->addLayout(file_row); settings->addLayout(log_box, 1);
    layout->addLayout(settings);

    auto* actions = new QHBoxLayout;
    start_ = new QPushButton("Start monitor"); start_->setObjectName("start");
    stop_ = new QPushButton("Stop and drain"); stop_->setObjectName("stop");
    demo_ = new QPushButton("Send demo readings"); demo_->setObjectName("demo");
    actions->addWidget(start_); actions->addWidget(stop_); actions->addWidget(demo_); actions->addStretch();
    address_ = new QLabel("Local TCP server"); address_->setObjectName("address"); actions->addWidget(address_);
    layout->addLayout(actions);

    table_ = new QTableWidget(0, 4); table_->setObjectName("readings");
    table_->setHorizontalHeaderLabels({"Device", "Latest value", "Sequence", "Received"});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    layout->addWidget(table_, 1);
    detail_ = new QLabel("Start the monitor, then send readings using the demo button or Python simulator.");
    detail_->setWordWrap(true); detail_->setObjectName("detail"); layout->addWidget(detail_);
    setStyleSheet(
        "QMainWindow, QWidget { background: #f4f7fb; color: #17263b; font-family: 'Segoe UI', 'DejaVu Sans'; font-size: 13px; }"
        "QLabel#title { font-size: 27px; font-weight: 700; }"
        "QLabel#state { background: #e1e8f1; border-radius: 12px; padding: 8px 16px; font-weight: 600; }"
        "QFrame#card { background: white; border: 1px solid #dce4ef; border-radius: 10px; }"
        "QFrame#card QLabel { background: transparent; }"
        "QLabel[metric=true] { font-size: 30px; font-weight: 700; color: #174c90; }"
        "QSpinBox, QLineEdit { background: white; border: 1px solid #c9d5e4; border-radius: 6px; padding: 8px; }"
        "QPushButton { background: white; border: 1px solid #c9d5e4; border-radius: 6px; padding: 10px 14px; }"
        "QPushButton#start { background: #175aa8; color: white; border-color: #175aa8; font-weight: 600; }"
        "QPushButton:hover { border-color: #175aa8; }"
        "QPushButton:disabled { background: #edf1f6; color: #8a96a7; border-color: #dce4ef; }"
        "QPushButton#start:disabled { background: #edf1f6; color: #8a96a7; border-color: #dce4ef; }"
        "QTableWidget { background: white; alternate-background-color: #f6f9fd; border: 1px solid #dce4ef; gridline-color: #e7edf5; }"
        "QHeaderView::section { background: #eaf0f8; border: 0; padding: 10px; font-weight: 600; }"
        "QLabel#detail, QLabel#address { color: #52647c; }"
    );

    refresh_timer_ = new QTimer(this); refresh_timer_->setInterval(100);
    connect(refresh_timer_, &QTimer::timeout, this, &Dashboard::refresh); refresh_timer_->start();
    demo_timer_ = new QTimer(this); demo_timer_->setInterval(100);
    demo_socket_ = new QTcpSocket(this);
    connect(start_, &QPushButton::clicked, this, &Dashboard::startServer);
    connect(stop_, &QPushButton::clicked, this, &Dashboard::stopServer);
    connect(demo_, &QPushButton::clicked, this, &Dashboard::startDemo);
    connect(browse_, &QPushButton::clicked, this, [this] {
        const auto selected = QFileDialog::getSaveFileName(this, "Choose a new capture file", log_->text(), "CSV files (*.csv)");
        if (!selected.isEmpty()) log_->setText(selected);
    });
    connect(demo_socket_, &QTcpSocket::connected, this, [this] { demo_timer_->start(); updateControls(); });
    connect(demo_socket_, &QTcpSocket::disconnected, this, [this] { demo_timer_->stop(); updateControls(); });
    connect(demo_socket_, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        detail_->setText("Demo connection failed: " + demo_socket_->errorString()); stopDemo();
    });
    connect(demo_timer_, &QTimer::timeout, this, [this] {
        if (demo_sequence_ == 30) { demo_timer_->stop(); demo_socket_->disconnectFromHost(); return; }
        const double value = 20.0 + 5.0 * std::sin(static_cast<double>(demo_sequence_) / 5.0);
        const QByteArray line = "qt-demo," + QByteArray::number(demo_sequence_++) + "," + QByteArray::number(value, 'f', 3) + "\n";
        if (demo_socket_->write(line) != line.size()) { detail_->setText("Demo write failed"); stopDemo(); }
    });
    updateControls();
}
Dashboard::~Dashboard() {
    state_->observer.stop_requested = true;
    if (thread_) { thread_->quit(); thread_->wait(); }
}
std::uint16_t Dashboard::listeningPort() const {
    std::lock_guard<std::mutex> lock(state_->mutex); return state_->port;
}
std::uint64_t Dashboard::processed() const {
    std::lock_guard<std::mutex> lock(state_->mutex); return state_->status.processed;
}
std::size_t Dashboard::deviceCount() const {
    std::lock_guard<std::mutex> lock(state_->mutex); return state_->latest.size();
}
double Dashboard::latestValue(const std::string& device) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->latest.find(device);
    return it == state_->latest.end() ? std::numeric_limits<double>::quiet_NaN() : it->second.value;
}
void Dashboard::startServer() {
    if (running()) return;
    const auto path = log_->text().trimmed();
    if (path.isEmpty() || QFileInfo::exists(path)) {
        state_label_->setText("Choose a new file");
        detail_->setText("Select a capture filename that does not already exist.");
        return;
    }
    state_ = std::make_shared<DashboardState>();
    stopping_ = false;
    telemetry::Config config;
    config.port = static_cast<std::uint16_t>(port_->value());
    config.queue_capacity = static_cast<std::size_t>(capacity_->value());
    config.log_path = QFileInfo(path).absoluteFilePath().toStdString();
    auto* worker = new ServerWorker(config, state_);
    auto* thread = new QThread(this); thread_ = thread;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &ServerWorker::run);
    connect(worker, &ServerWorker::completed, thread, &QThread::quit, Qt::DirectConnection);
    connect(worker, &ServerWorker::completed, this, [this](int code, const QString& error) {
        refresh();
        state_label_->setText(code == 0 ? "Stopped" : "Error");
        if (code == 0) detail_->setText("Capture complete. Accepted queued readings were drained to the CSV.");
        else detail_->setText("Monitor failed: " + error);
        emit serverCompleted(code);
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        stopDemo(); thread_ = nullptr; stopping_ = false; thread->deleteLater();
        refresh(); updateControls();
        if (close_requested_) QTimer::singleShot(0, this, &QWidget::close);
    });
    state_label_->setText("Starting"); detail_->setText("Starting the local TCP monitor…");
    table_->setRowCount(0); refresh(); updateControls(); thread->start();
}
void Dashboard::stopServer() {
    if (!running() || stopping_) return;
    stopping_ = true; state_->observer.stop_requested = true;
    stopDemo(); state_label_->setText("Draining");
    detail_->setText("Finishing accepted readings before stopping…"); updateControls();
}
void Dashboard::refresh() {
    std::map<std::string, telemetry::Reading> latest;
    telemetry::Status status;
    std::uint16_t port;
    bool limited;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        latest = state_->latest; status = state_->status; port = state_->port; limited = state_->display_limited;
    }
    connected_->setText(QString::number(status.connected));
    processed_->setText(QString::number(static_cast<qulonglong>(status.processed)));
    dropped_->setText(QString::number(static_cast<qulonglong>(status.dropped)));
    malformed_->setText(QString::number(static_cast<qulonglong>(status.malformed)));
    if (running() && !stopping_ && port != 0) {
        state_label_->setText("Listening"); address_->setText("127.0.0.1:" + QString::number(port));
        detail_->setText(QString("Queue high water: %1  ·  Oversized: %2  ·  Partial: %3%4")
            .arg(static_cast<qulonglong>(status.queue_high_water)).arg(static_cast<qulonglong>(status.oversized))
            .arg(static_cast<qulonglong>(status.truncated)).arg(limited ? "  ·  Display limited to 128 devices" : ""));
    } else if (!running()) address_->setText("Local TCP server");
    table_->setRowCount(static_cast<int>(latest.size()));
    int row = 0;
    for (const auto& entry : latest) {
        const auto& reading = entry.second;
        const QStringList values{QString::fromStdString(reading.device), QString::number(reading.value, 'g', 8),
            QString::number(static_cast<qulonglong>(reading.sequence)), QDateTime::fromMSecsSinceEpoch(reading.received_ms).toString("HH:mm:ss.zzz")};
        for (int column = 0; column < 4; ++column) {
            if (!table_->item(row, column)) table_->setItem(row, column, new QTableWidgetItem);
            table_->item(row, column)->setText(values[column]);
        }
        ++row;
    }
    updateControls();
}
void Dashboard::updateControls() {
    const bool active = running();
    start_->setEnabled(!active); stop_->setEnabled(active && !stopping_);
    port_->setEnabled(!active); capacity_->setEnabled(!active); log_->setEnabled(!active); browse_->setEnabled(!active);
    demo_->setEnabled(active && !stopping_ && listeningPort() != 0 && demo_socket_->state() == QAbstractSocket::UnconnectedState);
}
void Dashboard::startDemo() {
    if (!running() || stopping_ || listeningPort() == 0) return;
    demo_sequence_ = 0;
    demo_socket_->connectToHost("127.0.0.1", listeningPort()); updateControls();
}
void Dashboard::stopDemo() {
    demo_timer_->stop(); demo_socket_->abort(); updateControls();
}
void Dashboard::closeEvent(QCloseEvent* event) {
    if (running()) { close_requested_ = true; stopServer(); event->ignore(); }
    else event->accept();
}
