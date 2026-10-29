#include "desktop/update_install.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QtDebug>
#include <algorithm>

namespace {

#if defined(Q_OS_WIN)
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

bool makeParent(const QString& path) {
    return QDir().mkpath(QFileInfo(path).absolutePath());
}

} // namespace

Installation currentInstallation() {
    Installation installation;
    installation.executable = QCoreApplication::applicationFilePath();
    QDir directory = QFileInfo(installation.executable).absoluteDir();
#if defined(Q_OS_MACOS)
    // <name>.app/Contents/MacOS/<executable>
    directory.cdUp();
    directory.cdUp();
    installation.root = directory.absolutePath();
    installation.bundle = true;
    installation.workDir = QFileInfo(installation.root).absolutePath() + "/.disquisition-update";
#else
    installation.root = directory.absolutePath();
    installation.workDir = installation.root + "/.disquisition-update";
#endif
    return installation;
}

QList<Replacement> planReplacements(const Installation& installation, QString* error) {
    const QDir payload(installation.payloadDir());
    if (installation.bundle) {
        const QStringList apps = payload.entryList({"*.app"}, QDir::Dirs | QDir::NoDotAndDotDot);
        if (apps.size() != 1 ||
            !QFileInfo::exists(payload.filePath(apps.first()) + "/Contents/Info.plist")) {
            *error = "the update does not contain an app";
            return {};
        }
        return {
            Replacement{payload.filePath(apps.first()), installation.root,
                        installation.backupDir() + "/" + QFileInfo(installation.root).fileName()}};
    }

    QList<Replacement> replacements;
    QDirIterator files(payload.path(), QDir::Files | QDir::Hidden | QDir::System,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString relative = payload.relativeFilePath(files.next());
        replacements.append(Replacement{payload.filePath(relative),
                                        installation.root + "/" + relative,
                                        installation.backupDir() + "/" + relative});
    }
    // The app restarts from the same path, so the update must replace it.
    const QString executable =
        QDir(installation.root)
            .filePath(QDir(installation.root).relativeFilePath(installation.executable));
    const bool replacesExecutable =
        std::any_of(replacements.cbegin(), replacements.cend(), [&](const Replacement& r) {
            return r.installed.compare(executable, kPathCase) == 0;
        });
    if (!replacesExecutable) {
        *error = "the update does not contain " + QFileInfo(executable).fileName();
        return {};
    }
    return replacements;
}

bool applyReplacements(const QList<Replacement>& replacements, QString* error) {
    struct Step {
        const Replacement* replacement;
        bool backedUp = false;
        bool placed = false;
    };
    QList<Step> steps;
    QDir filesystem;

    const auto rollBack = [&](const QString& reason) {
        QStringList stuck;
        for (auto step = steps.crbegin(); step != steps.crend(); ++step) {
            const Replacement& r = *step->replacement;
            if (step->placed && !filesystem.rename(r.installed, r.staged)) {
                stuck.append(r.installed);
                continue;
            }
            if (step->backedUp && !filesystem.rename(r.backup, r.installed)) {
                stuck.append(r.backup);
            }
        }
        *error = reason;
        if (!stuck.isEmpty()) {
            *error += "; could not restore " + stuck.join(", ");
        }
        return false;
    };

    for (const Replacement& r : replacements) {
        Step& step = steps.emplaceBack(Step{&r});
        if (!makeParent(r.installed) || !makeParent(r.backup)) {
            return rollBack("could not create the folder for " + r.installed);
        }
        if (QFileInfo::exists(r.installed)) {
            if (!filesystem.rename(r.installed, r.backup)) {
                return rollBack("could not move " + r.installed + " aside");
            }
            step.backedUp = true;
        }
        if (!filesystem.rename(r.staged, r.installed)) {
            return rollBack("could not move the update into " + r.installed);
        }
        step.placed = true;
    }
    return true;
}

bool markStaged(const Installation& installation, int build) {
    QSaveFile marker(installation.stagedMarker());
    return marker.open(QIODevice::WriteOnly) && marker.write(QByteArray::number(build)) > 0 &&
           marker.commit();
}

std::optional<int> stagedBuild(const Installation& installation) {
    QFile marker(installation.stagedMarker());
    if (!marker.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    bool valid = false;
    const int build = marker.readAll().trimmed().toInt(&valid);
    return valid && build > 0 ? std::optional<int>(build) : std::nullopt;
}

bool installStaged(const Installation& installation, QString* error) {
    if (!QFile::remove(installation.stagedMarker())) {
        *error = "could not remove " + installation.stagedMarker();
        return false;
    }
    const QList<Replacement> replacements = planReplacements(installation, error);
    return !replacements.isEmpty() && applyReplacements(replacements, error);
}

QStringList relaunchArguments(QStringList currentArguments, const QDir& base) {
    const QString dataOption = QStringLiteral("--community-data=");
    for (qsizetype i = 0; i < currentArguments.size(); ++i) {
        if (currentArguments[i] == "--") {
            break;
        }
        if (currentArguments[i] == "--community-data" && i + 1 < currentArguments.size()) {
            ++i;
            currentArguments[i] = base.absoluteFilePath(currentArguments[i]);
        } else if (currentArguments[i].startsWith(dataOption)) {
            currentArguments[i] =
                dataOption + base.absoluteFilePath(currentArguments[i].mid(dataOption.size()));
        }
    }
    return currentArguments;
}

bool detachStaleUpdaterMount(const Installation& installation) {
#if defined(Q_OS_MACOS)
    const QString mount = installation.mountDir();
    QDir dir(mount);
    if (!dir.exists() || dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        return false;
    }
    QProcess::execute("/usr/bin/hdiutil", {"detach", mount, "-force"});
    return true;
#else
    Q_UNUSED(installation);
    return false;
#endif
}

bool launchInstallation(const Installation& installation) {
    const QStringList arguments =
        relaunchArguments(QCoreApplication::arguments().mid(1), QDir::current());
    if (installation.bundle) {
        // -n starts a new instance even though this one has not quit yet.
        QStringList openArguments{"-n", installation.root};
        if (!arguments.isEmpty()) {
            openArguments.append("--args");
            openArguments.append(arguments);
        }
        return QProcess::startDetached("/usr/bin/open", openArguments);
    }
    return QProcess::startDetached(installation.executable, arguments, QDir::currentPath());
}

bool startStagedUpdate(const Installation& installation, int currentBuild) {
    if (currentBuild <= 0) {
        return false;
    }
    const std::optional<int> build = stagedBuild(installation);
    if (!build) {
        return false;
    }
    if (*build <= currentBuild) {
        QFile::remove(installation.stagedMarker());
        return false;
    }
    QString error;
    if (!installStaged(installation, &error)) {
        qWarning().noquote() << "Could not install build" << *build << "-" << error;
        return false;
    }
    return launchInstallation(installation);
}
