#pragma once

#include <QList>
#include <QString>
#include <optional>

// Replacing the installed app with an extracted release.
//
// A release is downloaded and extracted while the app runs, then installed
// the next time it starts, before any window opens. Every step is a rename
// on one volume, so each installed file is either the old one or the new
// one, never a partial copy. Files the update replaces are moved aside
// rather than deleted, because Windows lets a program move its own
// executable and libraries while they are in use but not overwrite or
// delete them. They are deleted before the next update.

struct Installation {
    // On macOS, the .app bundle, replaced as a whole so its signature stays
    // valid. Elsewhere, the directory holding the executable, whose files
    // are replaced one at a time: Windows cannot move a directory while a
    // file in it is in use.
    QString root;
    bool bundle = false;
    QString executable;
    // Beside root, so it is on the same volume and every move is a rename.
    QString workDir;

    QString payloadDir() const { return workDir + "/payload"; }
    QString backupDir() const { return workDir + "/backup"; }
    QString stagedMarker() const { return workDir + "/staged"; }
};

Installation currentInstallation();

struct Replacement {
    QString staged;
    QString installed;
    QString backup;
};

// Pairs each path in the installation's payload with the installed path it
// replaces. Returns nothing, with `error` set, if the payload would not
// leave a runnable app in place.
QList<Replacement> planReplacements(const Installation& installation, QString* error);

// Moves every staged path into place. If one move fails, puts back
// everything already moved, so the previous installation is left as it
// was, and returns false with `error` set.
bool applyReplacements(const QList<Replacement>& replacements, QString* error);

// Records that the payload holds `build`, complete and checked, so that the
// next start installs it. Until then a payload is never installed.
bool markStaged(const Installation& installation, int build);
std::optional<int> stagedBuild(const Installation& installation);

// Installs the staged payload. Whether or not that succeeds, the payload is
// no longer staged, so an update that cannot be installed is not retried at
// every start.
bool installStaged(const Installation& installation, QString* error);

bool launchInstallation(const Installation& installation);

// For the start of the app, before any window opens. If an earlier session
// staged a build newer than `currentBuild`, installs and starts it, and
// returns true: the caller should exit so the new build takes over.
bool startStagedUpdate(const Installation& installation, int currentBuild);
