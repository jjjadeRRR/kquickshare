#include "senddialog.h"
#include "outboundconnection.h"
#include <KNotification>
#include <Prison/Barcode>
#include <QFileInfo>
#include <QHostInfo>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
SendDialog::SendDialog(const QStringList &files, const QString &text, QWidget *parent)
    : QDialog(parent)
    , m_discovery(this)
    , m_files(files)
    , m_text(text)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Send with KQuickShare"));
    resize(510, 650);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    auto *summary = new QLabel;
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    if (files.isEmpty())
        summary->setText(tr("Text: %1").arg(text.left(180)));
    else {
        QStringList names;
        for (const auto &path : files.mid(0, 4))
            names << QFileInfo(path).fileName();
        summary->setText(tr("Files: %1\n%2").arg(files.size()).arg(names.join('\n')));
    }
    layout->addWidget(summary);
    auto *hint = new QLabel(tr(
        "Scan the QR code with your phone camera and open the Quick Share link. Your phone and PC must be on the same network. Alternatively, open Files by Google → Quick Share → Receive on your phone."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_qrKey = crypto::generateP256();
    QByteArray x, y;
    if (m_qrKey && crypto::p256PublicXY(m_qrKey.get(), x, y)) {
        QByteArray key = QByteArray::fromHex("0000");
        key.append(char(2 | (quint8(y.back()) & 1)));
        key += x;
        m_discovery.setQrKey(key);
        const QString link = "https://quickshare.google/qrcode#key=" + QString::fromLatin1(base64UrlEncode(key));
        auto barcode = Prison::Barcode::create(Prison::QRCode);
        if (barcode) {
            barcode->setData(link);
            const auto image = barcode->toImage(QSizeF(210, 210));
            auto *qr = new QLabel;
            qr->setAlignment(Qt::AlignCenter);
            qr->setPixmap(QPixmap::fromImage(image));
            layout->addWidget(qr);
        }
    }
    m_list = new QListWidget;
    m_list->setObjectName("nearbyDevices");
    layout->addWidget(m_list, 1);
    m_pin = new QLabel;
    m_pin->setTextFormat(Qt::PlainText);
    layout->addWidget(m_pin);
    m_status = new QLabel(tr("Searching for phones on the local network…"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->hide();
    layout->addWidget(m_progress);
    auto *buttons = new QHBoxLayout;
    m_send = new QPushButton(QIcon::fromTheme("document-send"), tr("Send"));
    m_send->setObjectName("sendButton");
    m_send->setEnabled(false);
    buttons->addWidget(m_send);
    auto *close = new QPushButton(tr("Close"));
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &SendDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &SendDialog::send);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (m_send->isEnabled())
            send();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        m_send->setEnabled(!m_connection && row >= 0);
    });
    connect(&m_discovery, &DeviceDiscovery::changed, this, &SendDialog::updateDevices);
    connect(&m_discovery, &DeviceDiscovery::errorOccurred, m_status, &QLabel::setText);
    m_discovery.start();
}
SendDialog::~SendDialog()
{
    m_shuttingDown = true;
    if (m_connection) {
        // The finished handler updates widgets that are being destroyed.
        disconnect(m_connection, nullptr, this, nullptr);
        m_connection->cancel();
    }
}
void SendDialog::reject()
{
    m_shuttingDown = true;
    if (m_connection)
        m_connection->cancel();
    QDialog::reject();
}
void SendDialog::updateDevices()
{
    if (m_connection)
        return;
    const QString selected = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    m_devices = m_discovery.devices();
    m_list->clear();
    for (const auto &device : m_devices) {
        auto *item = new QListWidgetItem(QIcon::fromTheme("smartphone"), device.name + " · " + device.address.toString(), m_list);
        item->setData(Qt::UserRole, device.id);
        if (device.id == selected)
            m_list->setCurrentItem(item);
    }
    if (m_list->currentRow() < 0 && m_list->count() == 1)
        m_list->setCurrentRow(0);
    m_status->setText((m_lastError.isEmpty() ? QString() : m_lastError + "\n")
        + (m_devices.isEmpty() ? tr("Searching for phones… Open the Quick Share receive screen on your phone.") : tr("Select a phone and click Send.")));
}
void SendDialog::send()
{
    const int row = m_list->currentRow();
    if (m_connection || row < 0 || row >= m_devices.size())
        return;
    m_lastError.clear();
    m_pin->clear();
    m_progress->setValue(0);
    const auto device = m_devices[row];
    auto *connection = new OutboundConnection(this);
    QString error;
    const bool valid = m_files.isEmpty() ? connection->setText(m_text, error) : connection->setFiles(m_files, error);
    if (!valid) {
        m_status->setText(error);
        connection->deleteLater();
        return;
    }
    connection->setQrSigningKey(m_qrKey.get());
    m_connection = connection;
    m_discovery.stop();
    m_list->setEnabled(false);
    m_send->setEnabled(false);
    m_progress->show();
    connect(connection, &OutboundConnection::statusChanged, m_status, &QLabel::setText);
    connect(connection, &OutboundConnection::verificationCode, this, [this](const QString &pin) {
        m_pin->setText(tr("PIN %1 — compare the code on your phone").arg(pin));
    });
    connect(connection, &OutboundConnection::progress, this, [this](qint64 sent, qint64 total) {
        m_progress->setValue(int(1000.0 * sent / qMax(qint64(1), total)));
    });
    connect(connection, &OutboundConnection::finished, this, [this, connection, device](const QString &error) {
        m_connection = nullptr;
        connection->deleteLater();
        qInfo("Quick Share outbound finished: error=%s", qPrintable(error));
        m_status->setText(error.isEmpty() ? tr("Sent to «%1»").arg(device.name) : error);
        if (error.isEmpty())
            m_progress->setValue(1000);
        if (!error.isEmpty() && !m_shuttingDown) {
            m_lastError = error;
            m_send->setText(tr("Retry"));
            m_list->setEnabled(true);
            m_discovery.start();
            updateDevices();
        }
        auto *n = new KNotification(error.isEmpty() ? "complete" : "error", KNotification::CloseOnTimeout, this);
        n->setComponentName("kquickshare");
        n->setTitle("KQuickShare · " + device.name);
        n->setText(m_status->text().toHtmlEscaped());
        n->setIconName("document-send");
        n->sendEvent();
    });
    QSettings settings;
    connection->start(device.address, device.port, settings.value("name", QHostInfo::localHostName()).toString());
}

QString SendDialog::status() const
{
    return m_status->text();
}
