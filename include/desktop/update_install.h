#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

class QDir;

// Every install step is a rename. Windows lets a running program rename its
// own executable and libraries but not overwrite or delete them, so replaced
// files are moved aside and deleted before the next update.

struct Installation {
    // On macOS the .app bundle, swapped whole so its signature stays valid.
    QString root;
    bool bundle = false;
    QString executable;
    QString workDir;

    QString payloadDir() const { return workDir + "/payload"; }
    QString backupDir() const { return workDir + "/backup"; }
    QString stagedMarker() const { return workDir + "/staged"; }
    QString mountDir() const { return workDir + "/mount"; } // macOS DMG mount
};

Installation currentInstallation();

struct Replacement {
    QString staged;
    QString installed;
    QString backup;
};

QList<Replacement> planReplacements(const Installation& installation, QString* error);

// Rolls back everything already moved if one move fails.
bool applyReplacements(const QList<Replacement>& replacements, QString* error);

bool markStaged(const Installation& installation, int build);
std::optional<int> stagedBuild(const Installation& installation);

// Unstages the payload even on failure, so a bad update isn't retried every start.
bool installStaged(const Installation& installation, QString* error);

bool launchInstallation(const Installation& installation);

// Relative --community-data paths are made absolute against `base`:
// /usr/bin/open resets the working directory for the relaunched process.
QStringList relaunchArguments(QStringList currentArguments, const QDir& base);

// Detaches a hdiutil mount a crashed extraction may have left behind, which
// would otherwise block removeRecursively() on workDir. macOS only.
bool detachStaleUpdaterMount(const Installation& installation);

// Returns true if a newer staged build was installed and started; the caller should exit.
bool startStagedUpdate(const Installation& installation, int currentBuild);
