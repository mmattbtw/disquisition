#include "desktop/windows_notifications.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QStringView>
#include <atomic>

#include <windows.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <winstring.h>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;
namespace Notifications = ABI::Windows::UI::Notifications;
namespace Xml = ABI::Windows::Data::Xml::Dom;
using ActivatedHandler =
    ABI::Windows::Foundation::ITypedEventHandler<Notifications::ToastNotification*, IInspectable*>;

const QString kAppId = QStringLiteral("Disquisition.Desktop");
const QString kGroup = QStringLiteral("room");
constexpr int kIconSize = 256;
constexpr qsizetype kMaxTag = 64;

class HString {
public:
    explicit HString(QStringView text) {
        WindowsCreateString(reinterpret_cast<PCWSTR>(text.utf16()),
                            static_cast<UINT32>(text.size()), &value_);
    }
    ~HString() { WindowsDeleteString(value_); }
    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;

    HSTRING get() const { return value_; }

private:
    HSTRING value_ = nullptr;
};

// Windows calls this on a thread of its own.
class ToastActivated final : public ActivatedHandler {
public:
    explicit ToastActivated(WindowsNotifications* owner) : owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) ||
            iid == __uuidof(ActivatedHandler)) {
            *object = static_cast<ActivatedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Invoke(Notifications::IToastNotification*, IInspectable*) override {
        // The pointer is only read on the main thread.
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [owner = owner_] {
                if (owner) emit owner->activated();
            }, Qt::QueuedConnection);
        }
        return S_OK;
    }

private:
    QPointer<WindowsNotifications> owner_;
    std::atomic<ULONG> references_{1};
};

// Unpackaged apps need an AppUserModelID registered before Windows shows
// their toasts. It also names the app in them and in Settings.
void registerAppId(const QImage& icon) {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString iconPath = directory + "/notification-icon.png";
    QDir().mkpath(directory);
    icon.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(iconPath);
    QSettings registration("HKEY_CURRENT_USER\\Software\\Classes\\AppUserModelId\\" + kAppId,
                           QSettings::NativeFormat);
    registration.setValue("DisplayName", QGuiApplication::applicationDisplayName());
    registration.setValue("IconUri", QDir::toNativeSeparators(iconPath));
}

QString xmlText(const QString& text) {
    QString clean;
    clean.reserve(text.size());
    for (const QChar character : text) {
        // XML cannot hold other control characters, and LoadXml rejects them.
        if (character.unicode() >= 0x20 || character == u'\t' || character == u'\n') {
            clean.append(character);
        }
    }
    return clean.toHtmlEscaped();
}

QString toastXml(const QString& title, const QString& body) {
    return QStringLiteral("<toast><visual><binding template=\"ToastGeneric\">"
                          "<text>%1</text><text>%2</text>"
                          "</binding></visual>"
                          "<audio src=\"ms-winsoundevent:Notification.IM\"/></toast>")
        .arg(xmlText(title), xmlText(body));
}

QString tagFor(const QString& key) {
    if (key.size() <= kMaxTag) return key;
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

}  // namespace

struct WindowsNotifications::State {
    bool comInitialized = false;
    ComPtr<Notifications::IToastNotificationManagerStatics> manager;
    ComPtr<Notifications::IToastNotificationFactory> factory;
    ComPtr<Notifications::IToastNotifier> notifier;
    ComPtr<ActivatedHandler> activated;
};

WindowsNotifications::WindowsNotifications(const QImage& icon, QObject* parent) :
    QObject(parent), state_(std::make_unique<State>()) {
    // Matches the single-threaded apartment Qt has already set up here.
    state_->comInitialized = SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED));
    registerAppId(icon);
    if (FAILED(RoGetActivationFactory(
            HString(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).get(),
            IID_PPV_ARGS(&state_->manager))) ||
        FAILED(RoGetActivationFactory(
            HString(RuntimeClass_Windows_UI_Notifications_ToastNotification).get(),
            IID_PPV_ARGS(&state_->factory))) ||
        FAILED(state_->manager->CreateToastNotifierWithId(HString(kAppId).get(),
                                                          &state_->notifier))) {
        state_->notifier.Reset();
        return;
    }
    state_->activated.Attach(new ToastActivated(this));
}

WindowsNotifications::~WindowsNotifications() {
    const bool comInitialized = state_->comInitialized;
    state_.reset();
    if (comInitialized) RoUninitialize();
}

bool WindowsNotifications::isAvailable() const {
    return state_->notifier != nullptr;
}

void WindowsNotifications::show(const QString& key, const QString& title, const QString& body) {
    if (!state_->notifier) return;
    ComPtr<IInspectable> instance;
    ComPtr<Xml::IXmlDocument> document;
    ComPtr<Xml::IXmlDocumentIO> loader;
    ComPtr<Notifications::IToastNotification> toast;
    ComPtr<Notifications::IToastNotification2> tagged;
    EventRegistrationToken token;
    if (FAILED(RoActivateInstance(HString(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).get(),
                                  &instance)) ||
        FAILED(instance.As(&document)) || FAILED(document.As(&loader)) ||
        FAILED(loader->LoadXml(HString(toastXml(title, body)).get())) ||
        FAILED(state_->factory->CreateToastNotification(document.Get(), &toast)) ||
        FAILED(toast.As(&tagged)) ||
        // A toast with the same tag and group replaces the one before it.
        FAILED(tagged->put_Tag(HString(tagFor(key)).get())) ||
        FAILED(tagged->put_Group(HString(kGroup).get())) ||
        FAILED(toast->add_Activated(state_->activated.Get(), &token))) {
        return;
    }
    state_->notifier->Show(toast.Get());
}

void WindowsNotifications::removeAll() {
    ComPtr<Notifications::IToastNotificationManagerStatics2> manager;
    ComPtr<Notifications::IToastNotificationHistory> history;
    if (state_->notifier && SUCCEEDED(state_->manager.As(&manager)) &&
        SUCCEEDED(manager->get_History(&history))) {
        history->ClearWithId(HString(kAppId).get());
    }
}
