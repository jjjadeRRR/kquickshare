#include <KLocalizedQmlContext>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTest>
class ClipboardTests : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kquickshare")
public:
    QString received;
public slots:
    void SendText(const QString &text)
    {
        received = text;
    }
private slots:
    void selectedItemOverDbus()
    {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService("org.kde.kquickshare"));
        QVERIFY(bus.registerObject("/Receiver", this, QDBusConnection::ExportAllSlots));
        QQmlEngine engine;
        engine.addImportPath(QString(CLIPBOARD_QML_PATH));
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        component.loadUrl(QUrl::fromLocalFile(QString(SOURCE_DIR) + "/tests/clipboard_fixture.qml"));
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        QVERIFY(QMetaObject::invokeMethod(object.data(), "sendSelected"));
        QTRY_COMPARE_WITH_TIMEOUT(received, object->property("selectedText").toString(), 5000);
        bus.unregisterObject("/Receiver");
        bus.unregisterService("org.kde.kquickshare");
    }
};
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QDBusConnection::sessionBus().registerService("org.freedesktop.portal.Desktop");
    ClipboardTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "clipboard_tests.moc"
