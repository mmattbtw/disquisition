#include "community/store.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <stdexcept>

namespace community {
namespace {
void write(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(
            ("cannot save community data: " + file.errorString()).toStdString());
}
QByteArray read(const QString& path, qsizetype limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        throw std::runtime_error(("cannot read community data: " + path).toStdString());
    return file.readAll();
}
QJsonDocument document(const QByteArray& bytes) {
    QJsonParseError error;
    const auto result = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError)
        throw std::runtime_error("corrupt community data");
    return result;
}
} // namespace

Store::Store(const QString& directory) : directory_(directory) {
    if (!QDir().mkpath(directory))
        throw std::runtime_error("cannot create community data directory");
    lock_ = std::make_unique<QLockFile>(directory + "/session.lock");
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock())
        throw std::runtime_error("another client is using this community data directory");
    const auto path = directory + "/identity.key";
    if (QFile::exists(path))
        identity_ = Identity::fromSecret(read(path, 32));
    else {
        identity_ = Identity::generate();
        write(path, identity_.secret());
    }
}

QMap<QString, Model> Store::load() const {
    QMap<QString, Model> result;
    for (const auto& file : QDir(directory_).entryList({"*.community.json"}, QDir::Files)) {
        const auto model = Model::fromJson(
            document(read(directory_ + '/' + file, kMaxMetadataBytes)).object());
        if (file != model.id + ".community.json")
            throw std::runtime_error("community file identity mismatch");
        result.insert(model.id, model);
    }
    return result;
}
void Store::save(const Model& model) const {
    write(directory_ + '/' + model.id + ".community.json", canonical(model.toJson()));
}
void Store::savePending(const QJsonArray& invites) const {
    write(directory_ + "/pending.json", QJsonDocument(invites).toJson(QJsonDocument::Compact));
}
QJsonArray Store::pending() const {
    const auto path = directory_ + "/pending.json";
    return QFile::exists(path) ? document(read(path, 64 * 1024)).array() : QJsonArray{};
}
Profile Store::profile() const {
    const auto path = directory_ + "/profile.json";
    return QFile::exists(path)
               ? Profile::fromJson(document(read(path, 2 * kMaxAvatarBytes)).object())
               : Profile{};
}
void Store::saveProfile(const Profile& profile) const {
    Profile::fromJson(profile.toJson());
    write(directory_ + "/profile.json", canonical(profile.toJson()));
}
} // namespace community
