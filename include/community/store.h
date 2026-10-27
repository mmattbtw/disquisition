#pragma once

#include <QLockFile>
#include <memory>

#include "community/model.h"

namespace community {

// Metadata and identity persist independently of optional chat history.
class Store {
public:
    explicit Store(const QString& directory);
    const Identity& identity() const { return identity_; }
    QMap<QString, Model> load() const;
    void save(const Model& model) const;
    void savePending(const QJsonArray& invites) const;
    QJsonArray pending() const;
    Profile profile() const;
    void saveProfile(const Profile& profile) const;
    QString directory() const { return directory_; }

private:
    QString directory_;
    std::unique_ptr<QLockFile> lock_;
    Identity identity_;
};

} // namespace community
