#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QtEndian>

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstring>

namespace {
constexpr int kHeaderBytes = 8; // type(1) + packet_ts(4) + per_sample_ts_flag(1) + data_len(2)
constexpr int kChecksumBytes = 4;

quint32 calculateByteSumChecksum(const uchar *data, int length)
{
    quint32 sum = 0;
    for (int i = 0; i < length; ++i) {
        sum += data[i];
    }
    return sum;
}
}

class RealtimePlotWidget : public QWidget
{
public:
    explicit RealtimePlotWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(280);
        setAutoFillBackground(true);
    }

    void setSamples(const QVector<float> &samples)
    {
        m_samples = samples;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.fillRect(rect(), QColor(250, 252, 255));

        const QRectF plotRect = rect().adjusted(46, 16, -16, -34);
        painter.setPen(QPen(QColor(220, 228, 238), 1));
        painter.drawRect(plotRect);

        if (m_samples.isEmpty() || plotRect.width() <= 1.0 || plotRect.height() <= 1.0) {
            painter.setPen(QColor(95, 105, 120));
            painter.drawText(plotRect, Qt::AlignCenter, "Waiting for UDP samples...");
            return;
        }

        const auto minMax = std::minmax_element(m_samples.begin(), m_samples.end());
        float minY = *minMax.first;
        float maxY = *minMax.second;
        if (qFuzzyCompare(minY, maxY)) {
            minY -= 1.0f;
            maxY += 1.0f;
        }

        QPainterPath path;
        const int count = m_samples.size();
        for (int i = 0; i < count; ++i) {
            const qreal xRatio = (count > 1) ? static_cast<qreal>(i) / static_cast<qreal>(count - 1) : 0.0;
            const qreal x = plotRect.left() + xRatio * plotRect.width();

            const qreal yNorm = (static_cast<qreal>(m_samples.at(i)) - minY)
                / (static_cast<qreal>(maxY) - static_cast<qreal>(minY));
            const qreal y = plotRect.bottom() - yNorm * plotRect.height();

            if (i == 0) {
                path.moveTo(x, y);
            } else {
                path.lineTo(x, y);
            }
        }

        painter.setPen(QPen(QColor(22, 119, 255), 1.6));
        painter.drawPath(path);

        painter.setPen(QColor(75, 85, 95));
        painter.drawText(QRectF(6, plotRect.top() - 8, 36, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(maxY, 'g', 6));
        painter.drawText(QRectF(6, plotRect.bottom() - 8, 36, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(minY, 'g', 6));
        painter.drawText(QRectF(plotRect.left(), plotRect.bottom() + 8, plotRect.width(), 22),
                         Qt::AlignCenter,
                         QString("%1 samples shown").arg(count));
    }

private:
    QVector<float> m_samples;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_socket(nullptr)
    , m_plotTimer(new QTimer(this))
    , m_statsTimer(new QTimer(this))
    , m_groupEdit(nullptr)
    , m_portSpin(nullptr)
    , m_displayRateSpin(nullptr)
    , m_maxSamplesSpin(nullptr)
    , m_startStopButton(nullptr)
    , m_statusLabel(nullptr)
    , m_plotWidget(nullptr)
    , m_streaming(false)
    , m_plotDirty(false)
    , m_totalPackets(0)
    , m_badPackets(0)
    , m_totalSamples(0)
    , m_packetsSinceLastTick(0)
    , m_badPacketsSinceLastTick(0)
    , m_samplesSinceLastTick(0)
{
    ui->setupUi(this);
    buildUi();

    connect(m_plotTimer, &QTimer::timeout, this, &MainWindow::refreshPlot);
    connect(m_statsTimer, &QTimer::timeout, this, &MainWindow::updateStats);

    m_plotTimer->start(1000 / m_displayRateSpin->value());
    m_statsTimer->start(1000);

    updateStats();
}

MainWindow::~MainWindow()
{
    stopStreaming();
    delete ui;
}

void MainWindow::buildUi()
{
    setWindowTitle("UDP Multicast FPGA Plotter");

    auto *rootLayout = new QVBoxLayout(ui->centralwidget);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    auto *controlsBox = new QGroupBox("Receiver Settings", ui->centralwidget);
    auto *controlsLayout = new QGridLayout(controlsBox);

    m_groupEdit = new QLineEdit("239.10.10.10", controlsBox);
    m_portSpin = new QSpinBox(controlsBox);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(5000);

    m_displayRateSpin = new QSpinBox(controlsBox);
    m_displayRateSpin->setRange(5, 120);
    m_displayRateSpin->setValue(30);
    m_displayRateSpin->setSuffix(" Hz");

    m_maxSamplesSpin = new QSpinBox(controlsBox);
    m_maxSamplesSpin->setRange(200, 200000);
    m_maxSamplesSpin->setSingleStep(200);
    m_maxSamplesSpin->setValue(4000);

    m_startStopButton = new QPushButton("Start", controlsBox);

    controlsLayout->addWidget(new QLabel("Multicast Group:"), 0, 0);
    controlsLayout->addWidget(m_groupEdit, 0, 1);
    controlsLayout->addWidget(new QLabel("Port:"), 0, 2);
    controlsLayout->addWidget(m_portSpin, 0, 3);

    controlsLayout->addWidget(new QLabel("DataType in packet:"), 1, 0);
    controlsLayout->addWidget(new QLabel("0=U8, 1=U16, 2=U32"), 1, 1);
    controlsLayout->addWidget(new QLabel("Plot Refresh:"), 1, 2);
    controlsLayout->addWidget(m_displayRateSpin, 1, 3);

    controlsLayout->addWidget(new QLabel("History Length:"), 2, 0);
    controlsLayout->addWidget(m_maxSamplesSpin, 2, 1);
    controlsLayout->addWidget(m_startStopButton, 2, 3);

    m_statusLabel = new QLabel(controlsBox);
    controlsLayout->addWidget(m_statusLabel, 3, 0, 1, 4);

    m_plotWidget = new RealtimePlotWidget(ui->centralwidget);

    rootLayout->addWidget(controlsBox);
    rootLayout->addWidget(m_plotWidget, 1);

    connect(m_startStopButton, &QPushButton::clicked, this, &MainWindow::toggleStreaming);
    connect(m_displayRateSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &MainWindow::onDisplayRateChanged);
    connect(m_maxSamplesSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &MainWindow::onMaxSamplesChanged);
}

void MainWindow::toggleStreaming()
{
    if (m_streaming) {
        stopStreaming();
    } else {
        startStreaming();
    }
}

bool MainWindow::startStreaming()
{
    stopStreaming();

    QHostAddress groupAddress;
    if (!groupAddress.setAddress(m_groupEdit->text().trimmed())) {
        m_statusLabel->setText("Invalid multicast group address.");
        return false;
    }

    if (!groupAddress.isMulticast()) {
        m_statusLabel->setText("Address is not in multicast range.");
        return false;
    }

    m_socket = new QUdpSocket(this);
    const bool bindOk = m_socket->bind(QHostAddress::AnyIPv4,
                                       static_cast<quint16>(m_portSpin->value()),
                                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bindOk) {
        m_statusLabel->setText(QString("Bind failed: %1").arg(m_socket->errorString()));
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }

    if (!m_socket->joinMulticastGroup(groupAddress)) {
        m_statusLabel->setText(QString("Join multicast failed: %1").arg(m_socket->errorString()));
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }

    connect(m_socket, &QUdpSocket::readyRead, this, &MainWindow::processPendingDatagrams);

    m_joinedGroup = groupAddress;
    m_streaming = true;
    m_startStopButton->setText("Stop");
    m_groupEdit->setEnabled(false);
    m_portSpin->setEnabled(false);

    m_samples.clear();
    m_plotDirty = true;
    m_totalPackets = 0;
    m_badPackets = 0;
    m_totalSamples = 0;
    m_packetsSinceLastTick = 0;
    m_badPacketsSinceLastTick = 0;
    m_samplesSinceLastTick = 0;
    updateStats();
    return true;
}

void MainWindow::stopStreaming()
{
    if (m_socket) {
        m_socket->leaveMulticastGroup(m_joinedGroup);
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    m_joinedGroup = QHostAddress();
    m_streaming = false;
    if (m_startStopButton) {
        m_startStopButton->setText("Start");
    }
    if (m_groupEdit) {
        m_groupEdit->setEnabled(true);
    }
    if (m_portSpin) {
        m_portSpin->setEnabled(true);
    }
}

void MainWindow::processPendingDatagrams()
{
    if (!m_socket) {
        return;
    }

    while (m_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(datagram.data(), datagram.size());

        const int sampleCount = decodeSamples(datagram);
        if (sampleCount > 0) {
            m_totalSamples += static_cast<quint64>(sampleCount);
            m_samplesSinceLastTick += static_cast<quint64>(sampleCount);
        } else {
            ++m_badPackets;
            ++m_badPacketsSinceLastTick;
        }

        ++m_totalPackets;
        ++m_packetsSinceLastTick;
    }

    trimSampleBuffer();
    m_plotDirty = true;
}

int MainWindow::decodeSamples(const QByteArray &payload)
{
    const uchar *bytes = reinterpret_cast<const uchar *>(payload.constData());
    const int totalBytes = payload.size();

    if (totalBytes < (kHeaderBytes + kChecksumBytes)) {
        return -1;
    }

    const quint8 dataType = bytes[0];
    const quint32 packetTimestamp = qFromLittleEndian<quint32>(bytes + 1);
    const quint8 dataTimestampFlag = bytes[5];
    const quint16 dataLength = qFromLittleEndian<quint16>(bytes + 6);
    Q_UNUSED(packetTimestamp);

    const int expectedBytes = kHeaderBytes + static_cast<int>(dataLength);
    if (totalBytes != expectedBytes) {
        return -1;
    }
    if (dataLength < kChecksumBytes) {
        return -1;
    }
    if (dataTimestampFlag > 1) {
        return -1;
    }

    // Assumes checksum is a simple 32-bit sum of all bytes before checksum.
    const quint32 reportedChecksum = qFromLittleEndian<quint32>(bytes + (totalBytes - kChecksumBytes));
    const quint32 calculatedChecksum = calculateByteSumChecksum(bytes, totalBytes - kChecksumBytes);
    if (reportedChecksum != calculatedChecksum) {
        return -1;
    }

    int sampleBytes = 0;
    switch (dataType) {
    case 0:
        sampleBytes = 1;
        break;
    case 1:
        sampleBytes = 2;
        break;
    case 2:
        sampleBytes = 4;
        break;
    default:
        return -1;
    }

    const int perSampleTimestampBytes = (dataTimestampFlag == 1) ? 4 : 0;
    const int stride = sampleBytes + perSampleTimestampBytes;
    const int dataSectionBytes = static_cast<int>(dataLength) - kChecksumBytes;
    if (stride <= 0 || (dataSectionBytes % stride) != 0) {
        return -1;
    }

    const int sampleCount = dataSectionBytes / stride;
    int offset = kHeaderBytes;
    for (int i = 0; i < sampleCount; ++i) {
        if (dataTimestampFlag == 1) {
            const quint32 sampleTimestamp = qFromLittleEndian<quint32>(bytes + offset);
            Q_UNUSED(sampleTimestamp);
            offset += 4;
        }

        float sampleValue = 0.0f;
        if (dataType == 0) {
            sampleValue = static_cast<float>(bytes[offset]);
        } else if (dataType == 1) {
            sampleValue = static_cast<float>(qFromLittleEndian<quint16>(bytes + offset));
        } else {
            sampleValue = static_cast<float>(qFromLittleEndian<quint32>(bytes + offset));
        }

        m_samples.push_back(sampleValue);
        offset += sampleBytes;
    }

    return sampleCount;
}

void MainWindow::trimSampleBuffer()
{
    const int maxSamples = m_maxSamplesSpin->value();
    const int overflow = m_samples.size() - maxSamples;
    if (overflow > 0) {
        m_samples.remove(0, overflow);
    }
}

void MainWindow::refreshPlot()
{
    if (!m_plotDirty) {
        return;
    }

    m_plotWidget->setSamples(m_samples);
    m_plotDirty = false;
}

void MainWindow::updateStats()
{
    const QString streamState = m_streaming ? "RUNNING" : "STOPPED";
    m_statusLabel->setText(
        QString("%1  |  packets/s: %2  |  bad/s: %3  |  samples/s: %4  |  total packets: %5  |  bad total: %6  |  total samples: %7")
            .arg(streamState)
            .arg(m_packetsSinceLastTick)
            .arg(m_badPacketsSinceLastTick)
            .arg(m_samplesSinceLastTick)
            .arg(m_totalPackets)
            .arg(m_badPackets)
            .arg(m_totalSamples));

    m_packetsSinceLastTick = 0;
    m_badPacketsSinceLastTick = 0;
    m_samplesSinceLastTick = 0;
}

void MainWindow::onDisplayRateChanged(int hz)
{
    const int safeHz = std::max(1, hz);
    m_plotTimer->setInterval(1000 / safeHz);
}

void MainWindow::onMaxSamplesChanged(int samples)
{
    Q_UNUSED(samples);
    trimSampleBuffer();
    m_plotDirty = true;
}

