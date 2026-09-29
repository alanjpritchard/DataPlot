#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QHostAddress>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QUdpSocket;
class RealtimePlotWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void toggleStreaming();
    void processPendingDatagrams();
    void refreshPlot();
    void updateStats();
    void onDisplayRateChanged(int hz);
    void onMaxSamplesChanged(int samples);

private:
    void buildUi();
    bool startStreaming();
    void stopStreaming();
    int decodeSamples(const QByteArray &payload);
    void trimSampleBuffer();

    Ui::MainWindow *ui;

    QUdpSocket *m_socket;
    QTimer *m_plotTimer;
    QTimer *m_statsTimer;

    QLineEdit *m_groupEdit;
    QSpinBox *m_portSpin;
    QSpinBox *m_displayRateSpin;
    QSpinBox *m_maxSamplesSpin;
    QPushButton *m_startStopButton;
    QLabel *m_statusLabel;
    RealtimePlotWidget *m_plotWidget;

    QHostAddress m_joinedGroup;
    bool m_streaming;

    QVector<float> m_samples;
    bool m_plotDirty;

    quint64 m_totalPackets;
    quint64 m_badPackets;
    quint64 m_totalSamples;
    quint64 m_packetsSinceLastTick;
    quint64 m_badPacketsSinceLastTick;
    quint64 m_samplesSinceLastTick;
};
#endif // MAINWINDOW_H
