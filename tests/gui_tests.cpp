#include "dashboard.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class DashboardTests final : public QObject {
    Q_OBJECT
    static void configure(Dashboard& window, const QString& path) {
        window.findChild<QSpinBox*>("port")->setValue(0);
        window.findChild<QLineEdit*>("logPath")->setText(path);
    }
    static void press(Dashboard& window, const char* name) {
        QTest::mouseClick(window.findChild<QPushButton*>(name), Qt::LeftButton);
    }
    static int capturedRows(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return -1;
        return static_cast<int>(file.readAll().count('\n')) - 1;
    }
private slots:
    void initTestCase() {
        // Offscreen Windows sandboxes may not enumerate installed system fonts.
        // Allow an explicit test font; normal desktop and Linux CI use system fonts.
        const auto font_path = qEnvironmentVariable("QT_DASHBOARD_TEST_FONT");
        if (!font_path.isEmpty()) {
            const int id = QFontDatabase::addApplicationFont(font_path);
            QVERIFY(id >= 0);
            QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(id).first()));
        }
    }
    void liveReadingsAndGracefulStop() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Dashboard window; configure(window, dir.filePath("capture.csv")); window.show();
        QSignalSpy finished(&window, &Dashboard::serverCompleted);
        int heartbeats = 0;
        QTimer heartbeat; heartbeat.setInterval(20);
        connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; }); heartbeat.start();
        press(window, "start");
        QTRY_VERIFY_WITH_TIMEOUT(window.listeningPort() != 0, 5000);
        QVERIFY(!window.findChild<QPushButton*>("start")->isEnabled());
        QTcpSocket client; client.connectToHost("127.0.0.1", window.listeningPort());
        QVERIFY(client.waitForConnected(3000));
        client.write("temperature,1,");
        client.flush();
        QTest::qWait(40);
        client.write("12.5\npressure,1,98.2\ntemperature,2,15.25\nbad-record\n");
        client.flush();
        QTRY_COMPARE_WITH_TIMEOUT(window.processed(), std::uint64_t{3}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("malformed")->text(), QString("1"), 5000);
        QCOMPARE(window.deviceCount(), std::size_t{2});
        QCOMPARE(window.latestValue("temperature"), 15.25);
        QTRY_COMPARE(window.findChild<QTableWidget*>("readings")->rowCount(), 2);
        QVERIFY(heartbeats >= 5);
        const auto screenshot = qEnvironmentVariable("QT_DASHBOARD_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QApplication::processEvents();
            QVERIFY(window.grab().save(screenshot));
        }
        press(window, "stop");
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toInt(), 0);
        QCOMPARE(capturedRows(dir.filePath("capture.csv")), 3);
        QVERIFY(window.findChild<QPushButton*>("start")->isEnabled());
        QVERIFY(!window.findChild<QPushButton*>("stop")->isEnabled());
    }
    void builtInDemoAndRestart() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        Dashboard window; configure(window, dir.filePath("first.csv")); window.show();
        press(window, "start");
        QTRY_VERIFY_WITH_TIMEOUT(window.listeningPort() != 0, 5000);
        QTRY_VERIFY(window.findChild<QPushButton*>("demo")->isEnabled());
        press(window, "demo");
        QTRY_COMPARE_WITH_TIMEOUT(window.processed(), std::uint64_t{30}, 7000);
        QCOMPARE(window.deviceCount(), std::size_t{1});
        press(window, "stop");
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QCOMPARE(capturedRows(dir.filePath("first.csv")), 30);
        press(window, "start");
        QVERIFY(!window.running());
        QCOMPARE(window.findChild<QLabel*>("state")->text(), QString("Choose a new file"));
        configure(window, dir.filePath(QString::fromUtf8("second-é.csv")));
        press(window, "start");
        QTRY_VERIFY_WITH_TIMEOUT(window.listeningPort() != 0, 5000);
        QCOMPARE(window.processed(), std::uint64_t{0});
        QCOMPARE(window.deviceCount(), std::size_t{0});
        press(window, "stop");
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QCOMPARE(capturedRows(dir.filePath(QString::fromUtf8("second-é.csv"))), 0);
    }
    void startupFailureRecoversControls() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        Dashboard window; configure(window, dir.filePath("unused.csv")); window.show();
        window.findChild<QSpinBox*>("port")->setValue(occupied.serverPort());
        QSignalSpy finished(&window, &Dashboard::serverCompleted);
        press(window, "start");
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toInt(), 1);
        QCOMPARE(window.findChild<QLabel*>("state")->text(), QString("Error"));
        QVERIFY(window.findChild<QPushButton*>("start")->isEnabled());
        QVERIFY(window.findChild<QLabel*>("detail")->text().contains("Cannot bind"));
    }
    void displayBoundAndCloseWhileRunning() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        Dashboard window; configure(window, dir.filePath("bounded.csv")); window.show();
        window.findChild<QSpinBox*>("capacity")->setValue(1024);
        press(window, "start");
        QTRY_VERIFY_WITH_TIMEOUT(window.listeningPort() != 0, 5000);
        QTcpSocket client; client.connectToHost("127.0.0.1", window.listeningPort());
        QVERIFY(client.waitForConnected(3000));
        QByteArray readings;
        for (int i = 0; i < 160; ++i) readings += "device-" + QByteArray::number(i) + ",1,1\n";
        readings += "unfinished,1,";
        client.write(readings); client.flush();
        QTRY_COMPARE_WITH_TIMEOUT(window.processed(), std::uint64_t{160}, 5000);
        QCOMPARE(window.deviceCount(), DashboardState::max_devices);
        QTRY_VERIFY(window.findChild<QLabel*>("detail")->text().contains("128 devices"));
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QTRY_VERIFY(!window.isVisible());
        QCOMPARE(capturedRows(dir.filePath("bounded.csv")), 160);
    }
};
QTEST_MAIN(DashboardTests)
#include "gui_tests.moc"
