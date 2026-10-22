#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QWidget>
#include <cmath>
#include <limits>

#include "check.h"
#include "desktop/chat_color.h"
#include "desktop/penguin_overlay.h"
#include "desktop/penguin_sprites.h"

#if defined(Q_OS_MACOS)
#    import <AppKit/AppKit.h>
#elif defined(Q_OS_WIN)
#    include <qt_windows.h>
#endif

namespace {

QList<QWidget*> overlayWindows() {
    QList<QWidget*> result;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == "penguinOverlay") result.append(widget);
    }
    return result;
}

void rosterTracksMembershipWithoutResettingPeople() {
    PenguinScene scene;
    scene.setMembers({"matt", "jesse", "matt", ""});
    CHECK(scene.penguins().size() == 2);
    const auto color = scene.penguins()["matt"].color;
    const auto x = scene.penguins()["matt"].x;
    scene.showMessage("matt", "hello");
    scene.setMembers({"jesse", "matt", "cam"});
    CHECK(scene.penguins().size() == 3);
    CHECK(scene.penguins()["matt"].speech == "hello");
    CHECK(scene.penguins()["matt"].x == x);
    scene.setMembers({"jesse"});
    CHECK(!scene.penguins().contains("matt"));
    scene.setMembers({"matt"});
    CHECK(scene.penguins()["matt"].color == color);
    CHECK(scene.penguins()["matt"].speech.isEmpty());
    scene.setMembers({});
    CHECK(scene.penguins().isEmpty());
}

void speechReplacesExpiresAndRemainsPlainText() {
    PenguinScene scene;
    scene.setMembers({"matt"});
    scene.showMessage("unknown", "hello");
    CHECK(scene.penguins().size() == 1);
    CHECK(scene.penguins()["matt"].speech.isEmpty());
    scene.showMessage("matt", "  <b>hi</b>\nthere  ");
    CHECK(scene.penguins()["matt"].speech == "<b>hi</b> there");
    scene.advance(4, false);
    CHECK(!scene.penguins()["matt"].speech.isEmpty());
    scene.showMessage("matt", "replacement");
    scene.advance(2, false);
    CHECK(scene.penguins()["matt"].speech == "replacement");
    scene.showMessage("matt", " \n ");
    CHECK(scene.penguins()["matt"].speech == "replacement");
    scene.advance(20, false);
    CHECK(scene.penguins()["matt"].speech.isEmpty());
    scene.showMessage("matt", QString(179, 'a') + QString::fromUtf8("🐧") + QString(300, 'b'));
    const QString text = scene.penguins()["matt"].speech;
    CHECK(text.size() <= 181);
    CHECK(text.endsWith(QChar(0x2026)));
    CHECK(!text.at(text.size() - 2).isHighSurrogate());
    CHECK(scene.penguins()["matt"].speechSeconds <= 12);
}

void penguinsUseChatColorsWithoutResettingState() {
    PenguinScene scene;
    scene.setMembers({"matt", "jesse"});
    CHECK(scene.penguins()["matt"].color == QColor(135, 255, 175));
    scene.showMessage("matt", "hello");
    const double x = scene.penguins()["matt"].x;
    scene.setColor("matt", chatColor("pink"));
    CHECK(scene.penguins()["matt"].color == QColor(255, 135, 175));
    CHECK(scene.penguins()["jesse"].color == chatColor("mint"));
    CHECK(scene.penguins()["matt"].speech == "hello");
    CHECK(scene.penguins()["matt"].x == x);
    scene.setMembers({"matt", "jesse", "cam"});
    CHECK(scene.penguins()["matt"].color == chatColor("pink"));
    scene.setColor("matt", chatColor("208"));
    CHECK(scene.penguins()["matt"].color == QColor(255, 135, 0));
    scene.setColor("matt", chatColor("0"));
    CHECK(scene.penguins()["matt"].color == QColor(0, 0, 0));
    scene.setColor("matt", chatColor("255"));
    CHECK(scene.penguins()["matt"].color == QColor(238, 238, 238));
    scene.setColor("unknown", chatColor("pink"));
    scene.setColor("matt", QColor());
    CHECK(scene.penguins().size() == 3);
    CHECK(scene.penguins()["matt"].color == QColor(238, 238, 238));
    CHECK(chatColor("invalid") == chatColor("mint"));
    CHECK(chatColor("-1") == chatColor("mint"));
    CHECK(chatColor("256") == chatColor("mint"));
    scene.setMembers({});
    scene.setMembers({"matt"});
    CHECK(scene.penguins()["matt"].color == chatColor("mint"));
}

void movementStaysOnScreenAndPausesWhileSpeaking() {
    PenguinScene scene;
    scene.setSize({320, 300});
    scene.setMembers({"matt", "jesse"});
    double traveled = 0;
    for (int i = 0; i < 1200; ++i) {
        const double before = scene.penguins()["matt"].x;
        scene.advance(0.05, true);
        const double after = scene.penguins()["matt"].x;
        CHECK(after >= 40 && after <= 280);
        CHECK(std::abs(after - before) <= 1.401);
        traveled += std::abs(after - before);
    }
    CHECK(traveled > 10);
    const double x = scene.penguins()["matt"].x;
    scene.advance(2, false);
    CHECK(scene.penguins()["matt"].x == x);
    scene.showMessage("matt", "hello!");
    scene.advance(1, true);
    CHECK(scene.penguins()["matt"].x == x);
    scene.advance(std::numeric_limits<double>::quiet_NaN(), true);
    CHECK(std::isfinite(scene.penguins()["matt"].x));
    scene.advance(100000, true);
    CHECK(std::abs(scene.penguins()["matt"].x - x) <= 2.801);
    scene.setSize({1, 100});
    for (int i = 0; i < 100; ++i) scene.advance(0.1, true);
    CHECK(scene.penguins()["matt"].x == 0.5);
    scene.setSize({1920, 300});
    CHECK(scene.penguins()["matt"].x >= 40);
    CHECK(scene.penguins()["matt"].x <= 1880);
}

void inactivityExpiresPerPersonWithoutRosterRefreshesExtendingIt() {
    PenguinActivity activity;
    activity.setMembers({"matt", "jesse"}, 0);
    CHECK(activity.activeMembers(599999, 10).size() == 2);
    CHECK(activity.activeMembers(600000, 10).isEmpty());
    activity.noteActivity("matt", 600000);
    CHECK(activity.activeMembers(600000, 10) == QStringList({"matt"}));
    activity.setMembers({"matt", "jesse", "cam"}, 600001);
    CHECK(activity.activeMembers(600001, 10) == QStringList({"cam", "matt"}));
    CHECK(activity.activeMembers(1200000, 10) == QStringList({"cam"}));
    CHECK(activity.activeMembers(1200001, 10).isEmpty());
    CHECK(activity.activeMembers(1200001, 0).size() == 3);
    CHECK(activity.activeMembers(1200001, 30).size() == 3);
    activity.noteActivity("unknown", 1200001);
    CHECK(activity.activeMembers(1200001, 10).isEmpty());
    // Leaving forgets activity; rejoining starts a fresh grace period.
    activity.setMembers({}, 1200001);
    activity.noteActivity("matt", 1200002);
    CHECK(activity.activeMembers(1200002, 0).isEmpty());
    activity.setMembers({"matt"}, 1200003);
    CHECK(activity.activeMembers(1800002, 10) == QStringList({"matt"}));
    CHECK(activity.activeMembers(1800003, 10).isEmpty());
}

void inactivePenguinsReturnOnChatAndStayInactiveAcrossVisibilityChanges() {
    qint64 now = 1000000;
    PenguinOverlay overlay(nullptr, [&] { return now; });
    overlay.setWandering(false);
    overlay.setMembers({"matt", "jesse"});
    CHECK(!overlayWindows().isEmpty());
    now += 600000;
    QElapsedTimer wait;
    wait.start();
    while (!overlayWindows().isEmpty() && wait.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    CHECK(overlayWindows().isEmpty());
    overlay.setColor("matt", chatColor("pink"));
    overlay.showMessage("matt", " \n ");
    overlay.showMessage("unknown", "hello");
    CHECK(overlayWindows().isEmpty());
    overlay.showMessage("matt", "I'm back");
    CHECK(!overlayWindows().isEmpty());
    now += 600000;
    overlay.setEnabled(false);
    overlay.setEnabled(true);
    CHECK(overlayWindows().isEmpty());
    overlay.setMembers({"matt", "jesse", "cam"});
    CHECK(!overlayWindows().isEmpty());
    overlay.setMembers({"matt", "jesse"});
    CHECK(overlayWindows().isEmpty());
    overlay.setIdleTimeoutMinutes(0);
    CHECK(!overlayWindows().isEmpty());
    overlay.setIdleTimeoutMinutes(10);
    CHECK(overlayWindows().isEmpty());
    // Activity still counts while the desktop overlay is disabled.
    overlay.setEnabled(false);
    overlay.noteActivity("jesse");
    CHECK(overlayWindows().isEmpty());
    overlay.setEnabled(true);
    CHECK(!overlayWindows().isEmpty());
}

void windowsAreTransparentAndFollowOverlayLifecycle() {
    PenguinOverlay overlay;
    overlay.setWandering(false);
    CHECK(overlayWindows().isEmpty());
    overlay.setMembers({"matt", "jesse"});
    QApplication::processEvents();
    CHECK(overlayWindows().size() == QGuiApplication::screens().size());
    for (QWidget* window : overlayWindows()) {
        CHECK(window->isVisible());
        CHECK(!window->parentWidget());
        CHECK(window->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        CHECK(window->windowFlags().testFlag(Qt::WindowTransparentForInput));
        CHECK(window->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        CHECK(window->testAttribute(Qt::WA_TranslucentBackground));
        CHECK(window->testAttribute(Qt::WA_ShowWithoutActivating));
        CHECK(window->screen()->availableGeometry().contains(window->geometry()));
#if defined(Q_OS_MACOS)
        if (QGuiApplication::platformName() == "cocoa") {
            NSView* view = reinterpret_cast<NSView*>(window->winId());
            CHECK(view.window.ignoresMouseEvents);
            CHECK(!view.window.hidesOnDeactivate);
            CHECK(view.window.level == NSStatusWindowLevel);
            CHECK(view.window.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces);
            CHECK(view.window.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary);
        }
#endif
        const QImage image = window->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        CHECK(!image.isNull());
        CHECK(qAlpha(image.pixel(0, 0)) == 0);
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) opaque += qAlpha(image.pixel(x, y)) > 0;
        }
        CHECK(opaque > 1000); // Bundled sprites and names actually rendered.
    }
    QWidget* window = overlayWindows().first();
    const QImage mint = window->grab().toImage();
    overlay.setColor("matt", chatColor("pink"));
    const QImage pink = window->grab().toImage();
    CHECK(pink != mint); // Cached sprites must repaint in the new color.
    overlay.setColor("matt", QColor());
    CHECK(window->grab().toImage() == pink);
    overlay.setEnabled(false);
    CHECK(overlayWindows().isEmpty());
    overlay.showMessage("matt", "hidden");
    overlay.setEnabled(true);
    CHECK(!overlayWindows().isEmpty());
    CHECK(overlayWindows().first()->grab().toImage() == pink);
    overlay.setMembers({});
    CHECK(overlayWindows().isEmpty());
}

void spriteUploadsAreValidatedAndSavedIndependentlyOfOriginals() {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString path = directory.filePath("standing.png");
    QImage original(600, 300, QImage::Format_ARGB32);
    original.fill(Qt::transparent);
    original.setPixelColor(300, 150, Qt::red);
    CHECK(original.save(path));
    QString error;
    const QImage standing = PenguinSprites::readImage(path, error);
    CHECK(error.isEmpty());
    CHECK(standing.size() == QSize(152, 76));
    CHECK(standing.pixelColor(0, 0).alpha() == 0);
    CHECK(PenguinSprites::readImage(directory.filePath("missing.png"), error).isNull());
    CHECK(!error.isEmpty());
    QFile invalid(directory.filePath("invalid.png"));
    CHECK(invalid.open(QIODevice::WriteOnly));
    invalid.write("not an image");
    invalid.close();
    CHECK(PenguinSprites::readImage(invalid.fileName(), error).isNull());
    CHECK(!error.isEmpty());
    QImage tooWide(4097, 1, QImage::Format_ARGB32);
    tooWide.fill(Qt::red);
    CHECK(tooWide.save(path));
    CHECK(PenguinSprites::readImage(path, error).isNull());
    CHECK(error.contains("4096"));
    CHECK(invalid.open(QIODevice::WriteOnly));
    CHECK(invalid.resize(10 * 1024 * 1024 + 1));
    invalid.close();
    CHECK(PenguinSprites::readImage(invalid.fileName(), error).isNull());
    CHECK(error.contains("10 MB"));

    QImage walking(76, 152, QImage::Format_ARGB32);
    walking.fill(Qt::green);
    const QString settingsPath = directory.filePath("sprites.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        PenguinSprites {true, standing, walking}.save(settings);
    }
    CHECK(QFile::remove(path));
    QSettings settings(settingsPath, QSettings::IniFormat);
    auto saved = PenguinSprites::fromSettings(settings);
    CHECK(saved.enabled);
    CHECK(saved.complete());
    CHECK(saved.standing == standing);
    CHECK(saved.walking == walking);
    {
        PenguinSpriteEditor editor(saved);
        auto* choice = editor.findChild<QComboBox*>("customSpriteChoice");
        CHECK(choice);
        choice->setCurrentIndex(0);
        CHECK(!editor.sprites().enabled);
        CHECK(editor.sprites().complete());
        // Closing an editor without Save must leave persisted choices intact.
    }
    CHECK(PenguinSprites::fromSettings(settings).enabled);
    saved.enabled = false;
    saved.save(settings);
    saved = PenguinSprites::fromSettings(settings);
    CHECK(!saved.enabled);
    CHECK(saved.standing == standing && saved.walking == walking);
    settings.setValue("penguins/standingSprite", QByteArray("broken"));
    CHECK(!PenguinSprites::fromSettings(settings).complete());
}

int pixelsOfColor(const QImage& image, QColor color) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += image.pixelColor(x, y) == color;
        }
    }
    return count;
}

void customSpritesSwitchForWalkingAndSurviveOverlayRecreation() {
    QImage standing(152, 152, QImage::Format_ARGB32);
    standing.fill(Qt::red);
    QImage walking(152, 152, QImage::Format_ARGB32);
    walking.fill(Qt::green);
    PenguinOverlay overlay;
    overlay.setCustomSprites(standing, walking);
    overlay.setWandering(false);
    overlay.setMembers({"matt"});
    auto capture = [] { return overlayWindows().first()->grab().toImage(); };
    CHECK(pixelsOfColor(capture(), Qt::red) > 1000);
    CHECK(pixelsOfColor(capture(), Qt::green) == 0);
    overlay.setColor("matt", chatColor("pink"));
    CHECK(pixelsOfColor(capture(), Qt::red) > 1000);
    overlay.setEnabled(false);
    CHECK(overlayWindows().isEmpty());
    overlay.setEnabled(true);
    CHECK(pixelsOfColor(capture(), Qt::red) > 1000);
    overlay.setWandering(true);
    QElapsedTimer wait;
    wait.start();
    while (pixelsOfColor(capture(), Qt::green) == 0 && wait.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
#if defined(Q_OS_MACOS)
    const bool reduced = [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
#elif defined(Q_OS_WIN)
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    const bool reduced = !animations;
#else
    const bool reduced = false;
#endif
    if (!reduced) CHECK(pixelsOfColor(capture(), Qt::green) > 1000);
    overlay.showMessage("matt", "hello");
    CHECK(pixelsOfColor(capture(), Qt::green) == 0);
    CHECK(pixelsOfColor(capture(), Qt::red) > 1000);
    overlay.setCustomSprites(standing, {});
    CHECK(pixelsOfColor(capture(), Qt::red) == 0); // Incomplete pair uses penguins.
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    rosterTracksMembershipWithoutResettingPeople();
    speechReplacesExpiresAndRemainsPlainText();
    penguinsUseChatColorsWithoutResettingState();
    movementStaysOnScreenAndPausesWhileSpeaking();
    inactivityExpiresPerPersonWithoutRosterRefreshesExtendingIt();
    inactivePenguinsReturnOnChatAndStayInactiveAcrossVisibilityChanges();
    windowsAreTransparentAndFollowOverlayLifecycle();
    spriteUploadsAreValidatedAndSavedIndependentlyOfOriginals();
    customSpritesSwitchForWalkingAndSurviveOverlayRecreation();
    if (app.arguments().contains("--preview")) {
        // A local preview of the actual native windows, with no server or saved
        // settings. It closes itself rather than leaving an overlay running.
        PenguinOverlay overlay;
        overlay.setMembers({"matt", "jesse", "cam"});
        overlay.setColor("matt", chatColor("aqua"));
        overlay.setColor("jesse", chatColor("pink"));
        overlay.setColor("cam", chatColor("208"));
        QTimer::singleShot(1500, &overlay, [&] {
            overlay.showMessage("matt", "Anyone up for a game?");
            overlay.showMessage("jesse", "Meet me at the iceberg 🐧");
        });
        QTimer::singleShot(2500, &overlay, [&] {
            const QString capture = qEnvironmentVariable("PENGUIN_CAPTURE");
            if (!capture.isEmpty() && !overlayWindows().isEmpty()) {
                CHECK(overlayWindows().first()->grab().save(capture));
            }
        });
        QTimer::singleShot(15000, &app, &QApplication::quit);
        return app.exec();
    }
    std::puts("penguin overlay tests passed");
    return 0;
}
