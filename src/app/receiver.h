#pragma once
#include "inboundconnection.h"
#include "nearbyserver.h"
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSettings>
class QLabel;
class QLineEdit;
class QCheckBox;
class QListWidget;
class QVBoxLayout;
class QProgressBar;
class KStatusNotifierItem;
class KNotification;
class Receiver : public QMainWindow {
    Q_OBJECT
  public:
    explicit Receiver(const QString &destinationOverride = QString());
    ~Receiver() override;
    void reveal();
    void sendFiles(const QStringList &paths);
    void sendText(const QString &text);
    QString sendStatus() const;

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    struct Transfer {
        TransferRequest request;
        QPointer<QWidget> card;
        QPointer<QLabel> status;
        QPointer<QProgressBar> progress;
        QPointer<QWidget> consent;
        QPointer<KNotification> notification;
        QPointer<QCheckBox> remember;
        bool textSaveFailed = false;
        bool accepted = false;
    };
    void incoming(InboundConnection *conn, const TransferRequest &request);
    void accept(InboundConnection *conn);
    void finish(InboundConnection *conn, const QStringList &files, const QString &error);
    void refreshState();
    void refreshTrusted();
    void startReceiver();
    QString trustKey(const TransferRequest &request) const;
    QString destination() const;
    KNotification *notify(const QString &event, const QString &title, const QString &body,
                          bool persistent = false);
    QString m_destination;
    NearbyServer m_server;
    QSettings m_settings;
    KStatusNotifierItem *m_tray;
    QLabel *m_state;
    QLineEdit *m_name;
    QLineEdit *m_folder;
    QCheckBox *m_enabled;
    QCheckBox *m_autostart;
    QListWidget *m_trusted;
    QVBoxLayout *m_transfers;
    QLabel *m_empty;
    QHash<InboundConnection *, Transfer> m_active;
    QString m_lastError;
};
