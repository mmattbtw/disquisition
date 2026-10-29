#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <optional>

#include "check.h"
#include "desktop/update_install.h"
#include "desktop/update_release.h"

namespace {

const QString kRepository = "mmattbtw/disquisition";
const QString kAsset = "disquisition-desktop-linux-x64.tar.gz";

// Shaped like GitHub's response for a release the desktop workflow published.
QJsonObject releaseJson() {
    const auto asset = [](const QString& name) {
        return QJsonObject{{"name", name},
                           {"state", "uploaded"},
                           {"size", 3859575},
                           {"browser_download_url",
                            "https://github.com/mmattbtw/disquisition/releases/download/main-119/" +
                                name}};
    };
    return QJsonObject{{"tag_name", "main-119"},
                       {"draft", false},
                       {"prerelease", false},
                       {"assets",
                        QJsonArray{asset("disquisition-desktop-macos-arm64.dmg"), asset(kAsset)}}};
}

std::optional<Release> parse(const QJsonObject& json) {
    return parseLatestRelease(QJsonDocument(json).toJson(), kRepository, kAsset);
}

QJsonObject withAsset(const std::function<void(QJsonObject&)>& change) {
    QJsonObject json = releaseJson();
    QJsonArray assets = json["assets"].toArray();
    QJsonObject asset = assets[1].toObject();
    change(asset);
    assets[1] = asset;
    json["assets"] = assets;
    return json;
}

void write(const QString& path, const QByteArray& contents) {
    CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(contents) == contents.size());
}

QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

Installation directoryInstallation(const QTemporaryDir& temp) {
    Installation installation;
    installation.root = temp.filePath("app");
    installation.executable = installation.root + "/disquisition-desktop";
    installation.workDir = installation.root + "/.disquisition-update";
    return installation;
}

void testBuildNumbers() {
    CHECK(releaseBuildNumber("main-1") == 1);
    CHECK(releaseBuildNumber("main-119") == 119);
    CHECK(!releaseBuildNumber("main-0"));
    CHECK(!releaseBuildNumber("main-042"));
    CHECK(!releaseBuildNumber("main-"));
    CHECK(!releaseBuildNumber("main-12a"));
    CHECK(!releaseBuildNumber(" main-12"));
    CHECK(!releaseBuildNumber("v1.0"));
    // Too large for an int.
    CHECK(!releaseBuildNumber("main-12345678901"));

    CHECK(latestReleaseUrl(kRepository) ==
          QUrl("https://api.github.com/repos/mmattbtw/disquisition/releases/latest"));
}

void testParseRelease() {
    const std::optional<Release> release = parse(releaseJson());
    CHECK(release);
    CHECK(release->buildNumber == 119);
    CHECK(release->page == QUrl("https://github.com/mmattbtw/disquisition/releases/tag/main-119"));
    CHECK(release->assetUrl ==
          QUrl("https://github.com/mmattbtw/disquisition/releases/download/main-119/" + kAsset));
    CHECK(release->assetSize == 3859575);

    CHECK(!parseLatestRelease("not json", kRepository, kAsset));
    CHECK(!parseLatestRelease(QJsonDocument(releaseJson()).toJson(), kRepository,
                              "disquisition-desktop-windows-x64.zip"));

    QJsonObject json = releaseJson();
    json["tag_name"] = "v2.0";
    CHECK(!parse(json));
    json = releaseJson();
    json["draft"] = true;
    CHECK(!parse(json));
    json = releaseJson();
    json["prerelease"] = true;
    CHECK(!parse(json));

    // An asset still uploading, or without a size, is not downloaded.
    CHECK(!parse(withAsset([](QJsonObject& a) { a["state"] = "starter"; })));
    CHECK(!parse(withAsset([](QJsonObject& a) { a.remove("size"); })));

    // Downloads come only from this repository's release for the same tag.
    for (const QString& url :
         {"https://github.com/someone/disquisition/releases/download/main-119/" + kAsset,
          "https://github.com/mmattbtw/disquisition/releases/download/main-118/" + kAsset,
          "http://github.com/mmattbtw/disquisition/releases/download/main-119/" + kAsset,
          "https://github.com.example.com/mmattbtw/disquisition/releases/download/main-119/" +
              kAsset,
          "https://example.com/" + kAsset}) {
        CHECK(!parse(withAsset([&](QJsonObject& a) { a["browser_download_url"] = url; })));
    }
}

void testPlanDirectory() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const Installation installation = directoryInstallation(temp);
    write(installation.payloadDir() + "/disquisition-desktop", "new app");
    write(installation.payloadDir() + "/licenses/baresip.LICENSE", "new license");

    QString error;
    const QList<Replacement> plan = planReplacements(installation, &error);
    CHECK(plan.size() == 2);
    bool sawLicense = false;
    for (const Replacement& r : plan) {
        const QString relative = QDir(installation.payloadDir()).relativeFilePath(r.staged);
        CHECK(r.installed == installation.root + "/" + relative);
        CHECK(r.backup == installation.backupDir() + "/" + relative);
        sawLicense |= relative == "licenses/baresip.LICENSE";
    }
    CHECK(sawLicense);

    // An archive whose executable moved would leave the old one to restart.
    CHECK(QFile::remove(installation.payloadDir() + "/disquisition-desktop"));
    write(installation.payloadDir() + "/dist/disquisition-desktop", "new app");
    CHECK(planReplacements(installation, &error).isEmpty());
    CHECK(error.contains("disquisition-desktop"));
}

void testPlanBundle() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    Installation installation;
    installation.bundle = true;
    installation.root = temp.filePath("Applications/My disquisition.app");
    installation.executable = installation.root + "/Contents/MacOS/disquisition";
    installation.workDir = temp.filePath("Applications/.disquisition-update");

    QString error;
    CHECK(planReplacements(installation, &error).isEmpty());
    CHECK(!error.isEmpty());

    // The new app takes the installed bundle's name.
    write(installation.payloadDir() + "/disquisition.app/Contents/Info.plist", "plist");
    const QList<Replacement> plan = planReplacements(installation, &error);
    CHECK(plan.size() == 1);
    CHECK(plan[0].staged == installation.payloadDir() + "/disquisition.app");
    CHECK(plan[0].installed == installation.root);
    CHECK(plan[0].backup == installation.backupDir() + "/My disquisition.app");

    write(installation.payloadDir() + "/other.app/Contents/Info.plist", "plist");
    CHECK(planReplacements(installation, &error).isEmpty());
}

void testApplyReplacements() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const Installation installation = directoryInstallation(temp);
    write(installation.executable, "old app");
    write(installation.root + "/kept.txt", "kept");
    write(installation.payloadDir() + "/disquisition-desktop", "new app");
    write(installation.payloadDir() + "/plugins/new.so", "new plugin");

    QString error;
    const QList<Replacement> plan = planReplacements(installation, &error);
    CHECK(plan.size() == 2);
    CHECK(applyReplacements(plan, &error));
    CHECK(read(installation.executable) == "new app");
    CHECK(read(installation.root + "/plugins/new.so") == "new plugin");
    CHECK(read(installation.backupDir() + "/disquisition-desktop") == "old app");
    // Files the update does not include are left alone.
    CHECK(read(installation.root + "/kept.txt") == "kept");
    CHECK(!QFileInfo::exists(installation.payloadDir() + "/disquisition-desktop"));
}

void testApplyRollsBack() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const Installation installation = directoryInstallation(temp);
    write(installation.executable, "old app");
    write(installation.root + "/libqt.so", "old qt");
    write(installation.payloadDir() + "/disquisition-desktop", "new app");
    write(installation.payloadDir() + "/libqt.so", "new qt");

    QString error;
    QList<Replacement> plan = planReplacements(installation, &error);
    CHECK(plan.size() == 2);
    // The last move fails after the first has already happened.
    CHECK(QFile::remove(plan[1].staged));
    CHECK(!applyReplacements(plan, &error));
    CHECK(error.contains(plan[1].installed));
    CHECK(read(installation.executable) == "old app");
    CHECK(read(installation.root + "/libqt.so") == "old qt");
    // What did move is back in the payload.
    CHECK(read(plan[0].staged).startsWith("new "));
}

void testApplyReplacesDirectory() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const QString installed = temp.filePath("Applications/disquisition.app");
    const QString staged = temp.filePath("update/payload/disquisition.app");
    const QString backup = temp.filePath("update/backup/disquisition.app");
    write(installed + "/Contents/MacOS/disquisition", "old app");
    write(installed + "/Contents/Frameworks/Removed.framework", "old framework");
    write(staged + "/Contents/MacOS/disquisition", "new app");

    QString error;
    CHECK(applyReplacements({Replacement{staged, installed, backup}}, &error));
    CHECK(read(installed + "/Contents/MacOS/disquisition") == "new app");
    // A whole bundle is swapped, so nothing from the old one stays behind.
    CHECK(!QFileInfo::exists(installed + "/Contents/Frameworks/Removed.framework"));
    CHECK(read(backup + "/Contents/MacOS/disquisition") == "old app");
}

void testStagedUpdate() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const Installation installation = directoryInstallation(temp);
    write(installation.executable, "old app");
    write(installation.payloadDir() + "/disquisition-desktop", "new app");

    // A payload left by a session that stopped before finishing is ignored.
    CHECK(!stagedBuild(installation));
    CHECK(!startStagedUpdate(installation, 100));
    CHECK(read(installation.executable) == "old app");

    CHECK(markStaged(installation, 120));
    CHECK(stagedBuild(installation) == 120);
    // Builds that do not update themselves never install one.
    CHECK(!startStagedUpdate(installation, 0));
    CHECK(stagedBuild(installation) == 120);
    // Nor does a build already as new, which drops it.
    CHECK(!startStagedUpdate(installation, 120));
    CHECK(!stagedBuild(installation));
    CHECK(read(installation.executable) == "old app");

    CHECK(markStaged(installation, 121));
    QString error;
    CHECK(installStaged(installation, &error));
    CHECK(read(installation.executable) == "new app");
    CHECK(!stagedBuild(installation));

    write(installation.stagedMarker(), "main-121");
    CHECK(!stagedBuild(installation));
}

void testFailedInstallIsNotRetried() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const Installation installation = directoryInstallation(temp);
    write(installation.executable, "old app");
    write(installation.payloadDir() + "/somewhere/else", "new app");

    CHECK(markStaged(installation, 121));
    QString error;
    CHECK(!installStaged(installation, &error));
    CHECK(!error.isEmpty());
    CHECK(read(installation.executable) == "old app");
    CHECK(!stagedBuild(installation));
}

void testRelaunchArgumentsForwardsCommunityData() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const QDir base(temp.path());

    QStringList spaced =
        relaunchArguments({"--community-data", "relative/data", "disquisition://join/x"}, base);
    CHECK(spaced.size() == 3);
    CHECK(spaced[0] == "--community-data");
    CHECK(spaced[1] == base.absoluteFilePath("relative/data"));
    CHECK(spaced[2] == "disquisition://join/x");

    const QString absolute = base.absoluteFilePath("already/absolute");
    QStringList equals = relaunchArguments({"--community-data=also/rel",
                                            "--community-data=" + absolute},
                                           base);
    CHECK(equals.size() == 2);
    CHECK(equals[0] == "--community-data=" + base.absoluteFilePath("also/rel"));
    CHECK(equals[1] == "--community-data=" + absolute);

    QStringList passthrough =
        relaunchArguments({"--", "--community-data", "literal"}, base);
    CHECK(passthrough == QStringList({"--", "--community-data", "literal"}));

    CHECK(relaunchArguments({}, base).isEmpty());
}

void testDetachStaleUpdaterMount() {
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const Installation installation = directoryInstallation(temp);

    CHECK(!detachStaleUpdaterMount(installation));

    CHECK(QDir().mkpath(installation.mountDir()));
    CHECK(!detachStaleUpdaterMount(installation));

    write(installation.mountDir() + "/stale-marker", "placeholder");
#if !defined(Q_OS_MACOS)
    CHECK(!detachStaleUpdaterMount(installation));
#endif
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testBuildNumbers();
    testParseRelease();
    testPlanDirectory();
    testPlanBundle();
    testApplyReplacements();
    testApplyRollsBack();
    testApplyReplacesDirectory();
    testStagedUpdate();
    testFailedInstallIsNotRetried();
    testRelaunchArgumentsForwardsCommunityData();
    testDetachStaleUpdaterMount();
    std::puts("desktop_update_test passed");
    return 0;
}
