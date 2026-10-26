#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

#include "check.h"
#include "desktop/mentions.h"
#include "desktop/notification_rules.h"
#include "desktop/unread_badge.h"

namespace {

// The members each mention names, in order; "*" stands for @everyone and @here.
QStringList mentioned(const QString& body, const QStringList& members) {
    QStringList names;
    for (const Mention& mention : findMentions(body, members)) {
        names.append(mention.everyone() ? QStringLiteral("*") : mention.member);
    }
    return names;
}

void testFindMentions() {
    const QStringList room{"matt", "jesse", "matt-2", "jack!", "zoë"};

    QList<Mention> mentions = findMentions("hey @jesse, look", room);
    CHECK(mentions.size() == 1);
    CHECK(mentions[0].start == 4);
    CHECK(mentions[0].length == 6);
    CHECK(mentions[0].member == "jesse");
    CHECK(!mentions[0].everyone());

    CHECK(mentioned("@matt", room) == QStringList{"matt"});
    CHECK(mentioned("@matt and @jesse", room) == (QStringList{"matt", "jesse"}));
    CHECK(mentioned("(@matt)", room) == QStringList{"matt"});
    CHECK(mentioned("@matt's turn", room) == QStringList{"matt"});
    CHECK(mentioned("thanks @matt.", room) == QStringList{"matt"});
    CHECK(mentioned("@@matt", room) == QStringList{"matt"});
    // Case does not matter, including outside ASCII.
    CHECK(mentioned("@MATT", room) == QStringList{"matt"});
    CHECK(mentioned("@ZOË hi", room) == QStringList{"zoë"});
    // Names may end in punctuation that would otherwise end a mention.
    CHECK(mentioned("@jack! hi", room) == QStringList{"jack!"});
    // The longest name wins.
    CHECK(mentioned("@matt-2 hi", room) == QStringList{"matt-2"});

    // Not mentions: inside a word, a longer word, or nobody in the room.
    CHECK(mentioned("mail me@matt.net", room).isEmpty());
    CHECK(mentioned("a @mattress", room).isEmpty());
    CHECK(mentioned("@matt_", room).isEmpty());
    CHECK(mentioned("@nobody here", room).isEmpty());
    CHECK(mentioned("@", room).isEmpty());
    CHECK(mentioned("matt", room).isEmpty());
    CHECK(mentioned("", room).isEmpty());

    CHECK(mentioned("@everyone look", room) == QStringList{"*"});
    CHECK(mentioned("hi @here", room) == QStringList{"*"});
    CHECK(mentioned("@Everyone", room) == QStringList{"*"});
    CHECK(mentioned("@everyones", room).isEmpty());
    // @everyone wins a tie with someone who took the name.
    CHECK(mentioned("@everyone", {"everyone"}) == QStringList{"*"});
    CHECK(mentioned("@everyone2", {"everyone2"}) == QStringList{"everyone2"});

    // An exact match picks between names that differ only in case;
    // otherwise both are mentioned at the same place.
    const QStringList cased{"Matt", "matt"};
    CHECK(mentioned("@matt", cased) == QStringList{"matt"});
    CHECK(mentioned("@Matt", cased) == QStringList{"Matt"});
    mentions = findMentions("@MATT", cased);
    CHECK(mentions.size() == 2);
    CHECK(mentions[0].start == mentions[1].start && mentions[0].length == mentions[1].length);

    // A name containing '@' is one mention, not two.
    CHECK(mentioned("@a@b", {"a@b", "b"}) == QStringList{"a@b"});
}

void testMentionOf() {
    const QStringList room{"matt", "jesse"};
    CHECK(mentionOf(findMentions("@matt hi", room), "matt") == Mentioned::ByName);
    CHECK(mentionOf(findMentions("@jesse hi", room), "matt") == Mentioned::No);
    CHECK(mentionOf(findMentions("@here hi", room), "matt") == Mentioned::ViaEveryone);
    CHECK(mentionOf(findMentions("@everyone @matt", room), "matt") == Mentioned::ByName);
    CHECK(mentionOf(findMentions("@matt @everyone", room), "matt") == Mentioned::ByName);
    CHECK(mentionOf(findMentions("no mentions", room), "matt") == Mentioned::No);
    // Before signing in you have no name to be mentioned by.
    CHECK(mentionOf(findMentions("@everyone", room), "") == Mentioned::ViaEveryone);
    CHECK(mentionOf(findMentions("@matt", room), "") == Mentioned::No);
}

void testMentionQuery() {
    MentionQuery query = mentionQueryAt("hi @ma", 6);
    CHECK(query.start == 3);
    CHECK(query.prefix == "ma");
    query = mentionQueryAt("hi @ma", 4);
    CHECK(query.start == 3);
    CHECK(query.prefix.isEmpty());
    query = mentionQueryAt("@", 1);
    CHECK(query.start == 0);
    CHECK(query.prefix.isEmpty());
    // The cursor may sit inside a word.
    query = mentionQueryAt("@matt", 3);
    CHECK(query.start == 0);
    CHECK(query.prefix == "ma");

    CHECK(mentionQueryAt("hi @ma", 3).start == -1);
    CHECK(mentionQueryAt("@ma tt", 6).start == -1);
    CHECK(mentionQueryAt("me@ma", 5).start == -1);
    CHECK(mentionQueryAt("", 0).start == -1);
    // Out of range cursors are clamped rather than trusted.
    CHECK(mentionQueryAt("@ma", 99).prefix == "ma");
    CHECK(mentionQueryAt("@ma", -5).start == -1);
}

void testAttention() {
    NotificationSettings settings;
    MessageAttention attention = attentionFor(settings, Mentioned::No);
    CHECK(!attention.mention && attention.notify && !attention.alert);
    attention = attentionFor(settings, Mentioned::ByName);
    CHECK(attention.mention && attention.notify && attention.alert);
    attention = attentionFor(settings, Mentioned::ViaEveryone);
    CHECK(attention.mention && attention.notify && attention.alert);

    settings.level = NotifyLevel::MentionsOnly;
    attention = attentionFor(settings, Mentioned::No);
    CHECK(!attention.mention && !attention.notify && !attention.alert);
    attention = attentionFor(settings, Mentioned::ByName);
    CHECK(attention.mention && attention.notify && attention.alert);

    settings.suppressEveryone = true;
    attention = attentionFor(settings, Mentioned::ViaEveryone);
    CHECK(!attention.mention && !attention.notify && !attention.alert);
    // Suppressing @everyone never hides being named.
    attention = attentionFor(settings, Mentioned::ByName);
    CHECK(attention.mention && attention.notify && attention.alert);
    settings.level = NotifyLevel::AllMessages;
    attention = attentionFor(settings, Mentioned::ViaEveryone);
    CHECK(!attention.mention && attention.notify && !attention.alert);

    // Muting silences everything, but mentions still count toward the badge.
    settings = {};
    settings.muted = true;
    attention = attentionFor(settings, Mentioned::ByName);
    CHECK(attention.mention && !attention.notify && !attention.alert);
    attention = attentionFor(settings, Mentioned::No);
    CHECK(!attention.mention && !attention.notify && !attention.alert);

    settings = {};
    CHECK(notifiesActivity(settings));
    settings.activity = false;
    CHECK(!notifiesActivity(settings));
    settings.activity = true;
    settings.muted = true;
    CHECK(!notifiesActivity(settings));
}

void testSettings() {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString path = directory.filePath("settings.ini");

    {
        const QSettings empty(path, QSettings::IniFormat);
        CHECK(NotificationSettings::load(empty) == NotificationSettings{});
    }

    NotificationSettings chosen;
    chosen.muted = true;
    chosen.level = NotifyLevel::MentionsOnly;
    chosen.suppressEveryone = true;
    chosen.activity = false;
    {
        QSettings stored(path, QSettings::IniFormat);
        chosen.save(stored);
    }
    {
        const QSettings stored(path, QSettings::IniFormat);
        CHECK(NotificationSettings::load(stored) == chosen);
    }
    {
        QSettings stored(path, QSettings::IniFormat);
        stored.setValue("notifications/level", "nonsense");
    }
    const QSettings stored(path, QSettings::IniFormat);
    CHECK(NotificationSettings::load(stored).level == NotifyLevel::AllMessages);
}

void testNotificationText() {
    CHECK(notificationText("hello") == "hello");
    CHECK(notificationText("  two\tlines\nhere ") == "two lines here");

    const QString exact(kMaxNotificationText, u'a');
    CHECK(notificationText(exact) == exact);
    const QString shortened = notificationText(exact + "b");
    CHECK(shortened.size() == kMaxNotificationText);
    CHECK(shortened.endsWith(QChar(0x2026)));

    // A character outside the BMP straddling the cut is dropped whole.
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    CHECK(emoji.size() == 2);
    const QString straddling = QString(kMaxNotificationText - 2, u'a') + emoji + "tail";
    const QString cut = notificationText(straddling);
    CHECK(cut.size() == kMaxNotificationText - 1);
    CHECK(!cut.at(cut.size() - 2).isHighSurrogate());
    CHECK(cut.endsWith(QChar(0x2026)));
}

void testNotificationPacer() {
    NotificationPacer pacer(1000);
    const DesktopNotification first{"message/jesse", "jesse", "one"};
    const DesktopNotification second{"message/jesse", "jesse", "two"};
    const DesktopNotification third{"message/jesse", "jesse", "three"};
    const DesktopNotification other{"message/zoe", "zoe", "hi"};

    // The first of a burst shows straight away, as does another sender's.
    CHECK(!pacer.nextDue());
    CHECK(pacer.offer(first, 0));
    CHECK(pacer.offer(other, 10));

    // The rest wait for the cooldown, and only the latest shows.
    CHECK(!pacer.offer(second, 200));
    CHECK(!pacer.offer(third, 400));
    CHECK(pacer.nextDue() == 1000);
    CHECK(pacer.takeDue(999).isEmpty());
    const QList<DesktopNotification> due = pacer.takeDue(1000);
    CHECK(due.size() == 1);
    CHECK(due.first().body == "three");
    CHECK(!pacer.nextDue());

    // What showed late starts a new cooldown.
    CHECK(!pacer.offer(first, 1500));
    CHECK(pacer.nextDue() == 2000);
    CHECK(pacer.offer(second, 2000));
    CHECK(!pacer.nextDue());

    // A quiet sender shows straight away again.
    CHECK(pacer.offer(third, 5000));

    // Clearing drops what waits and forgets the cooldowns.
    CHECK(!pacer.offer(first, 5100));
    pacer.clear();
    CHECK(!pacer.nextDue());
    CHECK(pacer.takeDue(10000).isEmpty());
    CHECK(pacer.offer(second, 5200));
}

void testBadgeLabels() {
    CHECK(UnreadBadge{}.isEmpty());
    CHECK(UnreadBadge{}.label().isEmpty());
    CHECK((UnreadBadge{0, true}.label() == QString(QChar(0x2022))));
    CHECK((UnreadBadge{3, true}.label() == "3"));
    CHECK((UnreadBadge{99, true}.label() == "99"));
    CHECK((UnreadBadge{100, true}.label() == "99+"));

    CHECK(badgedTitle("disquisition", {}) == "disquisition");
    CHECK(badgedTitle("disquisition", {0, true}) == QString(QChar(0x2022)) + " disquisition");
    CHECK(badgedTitle("disquisition", {2, true}) == "(2) disquisition");
    CHECK(badgedTitle("disquisition", {250, true}) == "(99+) disquisition");
}

bool isRed(QRgb pixel) {
    return qRed(pixel) > 200 && qGreen(pixel) < 100 && qBlue(pixel) < 100;
}

bool isWhite(QRgb pixel) {
    return qRed(pixel) > 240 && qGreen(pixel) > 240 && qBlue(pixel) > 240;
}

void testDrawBadge() {
    QImage icon(64, 64, QImage::Format_ARGB32_Premultiplied);
    icon.fill(QColor(0x20, 0x60, 0x90));
    const QRgb background = icon.pixel(0, 0);

    CHECK(drawBadge(icon, {}) == icon);
    CHECK(drawBadge(QImage(), {1, true}).isNull());

    // A white dot in the lower right corner; the rest is untouched.
    const QImage dot = drawBadge(icon, {0, true});
    CHECK(dot.size() == icon.size());
    CHECK(isWhite(dot.pixel(51, 51)));
    CHECK(dot.pixel(5, 5) == background);
    CHECK(dot.pixel(60, 5) == background);

    // A red circle with the count. Sample above the digits.
    for (const int mentions : {1, 42}) {
        const QImage count = drawBadge(icon, {mentions, true});
        CHECK(isRed(count.pixel(45, 30)));
        CHECK(count.pixel(5, 5) == background);
        bool sawDigit = false;
        for (int y = 30; y < 60 && !sawDigit; ++y) {
            for (int x = 30; x < 60 && !sawDigit; ++x) sawDigit = isWhite(count.pixel(x, y));
        }
        CHECK(sawDigit);
    }

    // Tiny tray icons still get a visible badge.
    const QImage tiny = drawBadge(icon.scaled(16, 16), {0, true});
    CHECK(isWhite(tiny.pixel(13, 13)));
}

}  // namespace

int main(int argc, char** argv) {
    // Drawing text needs a GUI application, though not a display.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testFindMentions();
    testMentionOf();
    testMentionQuery();
    testAttention();
    testSettings();
    testNotificationText();
    testNotificationPacer();
    testBadgeLabels();
    testDrawBadge();
    std::puts("desktop_notifications_test passed");
    return 0;
}
