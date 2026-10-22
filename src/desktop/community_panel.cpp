#include "desktop/community_panel.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QFormLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "community/qr.h"
#include "desktop/audio_permission.h"
#include "desktop/chat_media.h"
#include "desktop/gif_picker.h"
#include "desktop/gif_search.h"
#include "desktop/remote_media.h"
#include "desktop/screen_stage.h"
#include "desktop/share_picker.h"

using community::freshId;
namespace {
QString key(const QString& cid, const QString& channel) {
    return cid + '/' + channel;
}
QString text(const std::string& value) {
    return QString::fromStdString(value);
}
QPushButton* button(const QString& title, const char* name, QWidget* parent) {
    auto* result = new QPushButton(title, parent);
    result->setObjectName(name);
    return result;
}
QList<community::Channel> orderedChannels(const community::Model& model) {
    auto channels = model.channels.values();
    std::sort(channels.begin(), channels.end(), [](const auto& a, const auto& b) {
        if (a.type != b.type)
            return a.type == "text";
        if (a.name == "general" || b.name == "general")
            return a.name == "general" && b.name != "general";
        const int order = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return order == 0 ? a.id < b.id : order < 0;
    });
    return channels;
}
QPixmap voiceIndicator(const QString& kind) {
    QPixmap image(16, 16);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(kind == "sharing screen" ? QColor("#9dccca") : QColor("#aebcbf"), 1.5));
    if (kind == "muted") {
        painter.drawRoundedRect(QRectF(6, 1, 4, 8), 2, 2);
        painter.drawArc(QRectF(4, 4, 8, 7), 180 * 16, 180 * 16);
        painter.drawLine(8, 11, 8, 14);
        painter.drawLine(5, 14, 11, 14);
    }
    else if (kind == "deafened") {
        painter.drawArc(QRectF(3, 2, 10, 12), 0, 180 * 16);
        painter.drawRoundedRect(QRectF(2, 8, 3, 6), 1, 1);
        painter.drawRoundedRect(QRectF(11, 8, 3, 6), 1, 1);
    }
    else {
        painter.drawRect(QRectF(2, 2, 12, 9));
        painter.drawLine(8, 11, 8, 14);
        painter.drawLine(5, 14, 11, 14);
    }
    if (kind != "sharing screen")
        painter.drawLine(2, 1, 14, 15);
    return image;
}
} // namespace

CommunityPanel::CommunityPanel(QWidget* parent, const QString& directory) : QWidget(parent) {
    try {
        const auto override = qApp->property("communityDataDirectory").toString();
        const auto path = !directory.isEmpty() ? directory
                          : !override.isEmpty()
                              ? override
                              : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                    "/communities";
        node_ = std::make_unique<community::Node>(path, this);
    }
    catch (const std::exception& error) {
        failure_ = QString::fromUtf8(error.what());
    }
    buildUi();
    imagePool_.setMaxThreadCount(1);
    if (node_) {
        connect(node_.get(), &community::Node::changed, this, [this] {
            if (refreshQueued_)
                return;
            refreshQueued_ = true;
            QTimer::singleShot(0, this, [this] {
                refreshQueued_ = false;
                refresh();
            });
        });
        connect(node_.get(), &community::Node::joined, this, [this](const QString& cid) {
            selectCommunity(cid);
            note("Community connected. Messages are delivered live.");
        });
        connect(node_.get(), &community::Node::joinRejected, this,
                [this](const QString&, const QString&) {
                    note("Invite rejected. Check the code with the community owner.");
                });
        connect(node_.get(), &community::Node::errorOccurred, this, &CommunityPanel::note);
        connect(node_.get(), &community::Node::packetReceived, this, &CommunityPanel::receive);
    }
    qApp->installEventFilter(this);
    imageTimer_.setInterval(2);
    connect(&imageTimer_, &QTimer::timeout, this, [this] {
        if (!online_ || uploads_.isEmpty()) {
            imageTimer_.stop();
            return;
        }
        if (node_->mediaBacklog() > 64 * 1024)
            return;
        auto& upload = uploads_.first();
        attempt([&] {
            node_->sendPacket(upload.cid, upload.channel, "image",
                              {{"transfer", double(upload.transfer)},
                               {"index", upload.next},
                               {"count", upload.chunks.size()},
                               {"bytes",
                                QString::fromLatin1(upload.chunks[upload.next].toBase64())}});
        });
        if (++upload.next == upload.chunks.size())
            uploads_.removeFirst();
    });
    connect(&audio_, &RelayAudio::frameReady, this, [this](const QByteArray& pcm) {
        if (online_ && !voiceCid_.isEmpty())
            attempt([&] {
                node_->sendPacket(voiceCid_, voiceChannel_, "audio",
                                  {{"bytes", QString::fromLatin1(pcm.toBase64())}});
            });
    });
    capture_.setBacklogProbe([this] { return node_ ? node_->mediaBacklog() : qint64(0); });
    connect(&capture_, &ScreenCapture::frameReady, this,
            [this](quint32 frame, const QList<QByteArray>& chunks, bool keyframe) {
                if (!online_ || voiceCid_.isEmpty())
                    return;
                QByteArray encoded;
                for (const auto& chunk : chunks)
                    encoded += chunk;
                auto image = decoders_[node_->peerId()].decode(frame, keyframe, encoded);
                if (!image.isNull())
                    stage_->showFrame(node_->peerId(), image, encoded.size());
                const auto watchers = watchers_;
                for (const auto& peer : watchers)
                    for (int index = 0; index < chunks.size(); ++index)
                        attempt([&] {
                            node_->sendPacket(voiceCid_, voiceChannel_, "screen",
                                              {{"target", peer},
                                               {"frame", double(frame)},
                                               {"index", index},
                                               {"count", chunks.size()},
                                               {"keyframe", keyframe},
                                               {"bytes",
                                                QString::fromLatin1(chunks[index].toBase64())}});
                        });
            });
    connect(&capture_, &ScreenCapture::errorOccurred, this, [this](const QString& error) {
        stopSharing();
        note(error);
    });
    refresh();
    render();
    for (const auto& argument : QCoreApplication::arguments())
        if (argument.startsWith("disquisition://join/"))
            pendingLink_ = argument;
}

CommunityPanel::~CommunityPanel() {
    qApp->removeEventFilter(this);
    leaveVoice();
}

void CommunityPanel::attempt(const std::function<void()>& action) {
    try {
        action();
    }
    catch (const std::exception& error) {
        note(QString::fromUtf8(error.what()));
    }
}
void CommunityPanel::note(const QString& message) {
    status_->setText(message);
    status_->setToolTip(message);
}

void CommunityPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    auto* connection = new QHBoxLayout;
    name_ = new QLineEdit(this);
    name_->setObjectName("communityName");
    name_->setPlaceholderText("Your name");
    name_->setText(QSettings().value("community/name").toString());
    connect_ = button("Connect", "communityConnect", this);
    auto* settingsButton = button("Connection && saving", "communitySettings", this);
    connection->addWidget(name_, 1);
    connection->addWidget(connect_);
    connection->addWidget(settingsButton);
    layout->addLayout(connection);
    connect(connect_, &QPushButton::clicked, this, &CommunityPanel::connectNode);
    connect(name_, &QLineEdit::returnPressed, this, &CommunityPanel::connectNode);
    connect(settingsButton, &QPushButton::clicked, this, &CommunityPanel::settings);
    auto* split = new QSplitter(this);
    auto* sidebar = new QWidget(split);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(0, 0, 4, 0);
    auto* actions = new QHBoxLayout;
    create_ = button("Create", "communityCreate", sidebar);
    join_ = button("Join", "communityJoin", sidebar);
    actions->addWidget(create_);
    actions->addWidget(join_);
    side->addLayout(actions);
    tree_ = new QTreeWidget(sidebar);
    tree_->setObjectName("communityTree");
    tree_->setHeaderHidden(true);
    tree_->setMinimumWidth(245);
    tree_->setIndentation(16);
    side->addWidget(tree_, 1);
    connect(create_, &QPushButton::clicked, this, &CommunityPanel::create);
    connect(join_, &QPushButton::clicked, this, &CommunityPanel::join);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item) { select(item); });
    auto* conversation = new QWidget(split);
    auto* chat = new QVBoxLayout(conversation);
    chat->setContentsMargins(4, 0, 0, 0);
    auto* headingRow = new QHBoxLayout;
    heading_ = new QLabel("Your communities", conversation);
    heading_->setStyleSheet("font-size:17px; font-weight:700;");
    heading_->setTextFormat(Qt::PlainText);
    invite_ = button("Invite", "communityInvite", conversation);
    manage_ = button("Manage", "communityManage", conversation);
    headingRow->addWidget(heading_, 1);
    headingRow->addWidget(invite_);
    headingRow->addWidget(manage_);
    chat->addLayout(headingRow);
    topic_ = new QLabel("Create a community or join with an invite link or QR image.",
                        conversation);
    topic_->setTextFormat(Qt::PlainText);
    topic_->setWordWrap(true);
    topic_->setStyleSheet("color:#9aabb4;");
    chat->addWidget(topic_);
    connect(invite_, &QPushButton::clicked, this, &CommunityPanel::invite);
    connect(manage_, &QPushButton::clicked, this, &CommunityPanel::manage);
    auto* content = new QSplitter(conversation);
    auto* center = new QSplitter(Qt::Vertical, content);
    stage_ = new ScreenStage(center);
    transcript_ = new QTextBrowser(center);
    transcript_->setObjectName("communityTranscript");
    transcript_->setMinimumHeight(120);
    center->addWidget(stage_);
    center->addWidget(transcript_);
    center->setStretchFactor(0, 2);
    center->setStretchFactor(1, 1);
    members_ = new QListWidget(content);
    members_->setObjectName("communityMembers");
    members_->setMinimumWidth(190);
    content->addWidget(center);
    content->addWidget(members_);
    content->setStretchFactor(0, 1);
    content->setSizes({600, 190});
    chat->addWidget(content, 1);
    split->addWidget(sidebar);
    split->addWidget(conversation);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 760});
    layout->addWidget(split, 1);
    voiceControls_ = new QWidget(this);
    voiceControls_->setObjectName("communityVoiceControls");
    auto* voiceRow = new QHBoxLayout(voiceControls_);
    voiceRow->setContentsMargins(10, 6, 10, 6);
    voiceStatus_ = new QLabel(voiceControls_);
    voiceStatus_->setTextFormat(Qt::PlainText);
    voice_ = button("Leave voice", "communityVoice", voiceControls_);
    mute_ = button("Mute", "communityMute", voiceControls_);
    deafen_ = button("Deafen", "communityDeafen", voiceControls_);
    share_ = button("Share screen", "communityShare", voiceControls_);
    mute_->setCheckable(true);
    deafen_->setCheckable(true);
    voiceRow->addWidget(voiceStatus_, 1);
    voiceRow->addWidget(voice_);
    voiceRow->addWidget(mute_);
    voiceRow->addWidget(deafen_);
    voiceRow->addWidget(share_);
    layout->addWidget(voiceControls_);
    voiceControls_->hide();
    connect(voice_, &QPushButton::clicked, this, &CommunityPanel::leaveVoice);
    connect(mute_, &QPushButton::toggled, this, &CommunityPanel::voiceState);
    connect(deafen_, &QPushButton::toggled, this, &CommunityPanel::voiceState);
    connect(share_, &QPushButton::clicked, this, &CommunityPanel::share);
    connect(stage_, &ScreenStage::watchRequested, this,
            [this](const QString& peer) { watch(peer, true); });
    connect(stage_, &ScreenStage::stopWatchingRequested, this,
            [this](const QString& peer) { watch(peer, false); });
    auto* compose = new QHBoxLayout;
    composer_ = new QLineEdit(this);
    composer_->setObjectName("communityComposer");
    composer_->setPlaceholderText("Message this channel");
    send_ = button("Send", "communitySend", this);
    image_ = button("Image", "communityImage", this);
    gif_ = button("GIF", "communityGif", this);
    compose->addWidget(composer_, 1);
    compose->addWidget(image_);
    compose->addWidget(gif_);
    compose->addWidget(send_);
    layout->addLayout(compose);
    connect(send_, &QPushButton::clicked, this, &CommunityPanel::sendChat);
    connect(composer_, &QLineEdit::returnPressed, this, &CommunityPanel::sendChat);
    connect(image_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, "Send an image", {}, "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)");
        if (!path.isEmpty())
            prepareUpload([path] { return prepareImageFile(path); });
    });
    status_ = new QLabel(this);
    status_->setObjectName("communityStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    status_->setTextFormat(Qt::PlainText);
    auto* network = new QNetworkAccessManager(this);
    auto* fetcher = new MediaFetcher(network, this);
    media_ = new ChatMedia(transcript_, fetcher, this);
    gifSearch_ = new GifSearch(network, this);
    QSettings settings;
    auto customer = settings.value("gifs/klipyCustomerId").toString();
    if (customer.isEmpty()) {
        customer = freshId();
        settings.setValue("gifs/klipyCustomerId", customer);
    }
    gifSearch_->setKeys(DISQUISITION_GIPHY_API_KEY, DISQUISITION_KLIPY_API_KEY, customer);
    gifPicker_ = new GifPicker(gifSearch_, fetcher, this);
    connect(gif_, &QPushButton::clicked, this, [this] { gifPicker_->popup(gif_); });
    connect(gifPicker_, &GifPicker::chosen, this, [this](const QUrl& url) {
        composer_->setText(url.toString());
        sendChat();
    });
    gif_->setVisible(gifSearch_->hasKey(GifProvider::Giphy) ||
                     gifSearch_->hasKey(GifProvider::Klipy));
    setAcceptDrops(true);
}

void CommunityPanel::connectNode() {
    if (!node_) {
        note(failure_);
        return;
    }
    if (online_) {
        leaveVoice();
        node_->stop();
        online_ = false;
        uploads_.clear();
        images_.clear();
        note("Disconnected. Saved communities remain available.");
    }
    else {
        QSettings settings;
        const auto port = settings.value("community/port", 0).toUInt();
        online_ =
            node_->start(name_->text().trimmed(), settings.value("community/host").toString(),
                         static_cast<quint16>(port),
                         settings.value("community/relay", "relay.disquisition.app").toString());
        if (online_) {
            settings.setValue("community/name", name_->text().trimmed());
            openHistory();
            const QUrl endpoint(node_->endpoint());
            note(endpoint.scheme() == "relay"
                     ? "Using relay " + endpoint.authority() +
                           ". Communities reconnect automatically."
                     : "Listening for direct connections at " + endpoint.authority());
            if (!pendingLink_.isEmpty()) {
                const auto link = pendingLink_;
                pendingLink_.clear();
                acceptInvite(link);
            }
        }
    }
    refresh();
}

void CommunityPanel::settings() {
    QSettings saved;
    QDialog dialog(this);
    dialog.setWindowTitle("Community connection & local saving");
    auto* form = new QFormLayout(&dialog);
    auto relayAddress = saved.value("community/relay", "relay.disquisition.app").toString();
    if (relayAddress.startsWith("relay://"))
        relayAddress.remove(0, 8);
    auto* relay = new QLineEdit(relayAddress, &dialog);
    relay->setObjectName("communityRelayAddress");
    relay->setPlaceholderText("relay.disquisition.app, or leave blank for direct connections");
    auto* host = new QLineEdit(saved.value("community/host").toString(), &dialog);
    host->setPlaceholderText("Automatic LAN address");
    auto* port = new QSpinBox(&dialog);
    port->setRange(0, 65535);
    port->setSpecialValueText("Automatic");
    port->setValue(saved.value("community/port", 0).toInt());
    relay->setEnabled(!online_);
    host->setEnabled(!online_);
    port->setEnabled(!online_);
    form->addRow("Relay", relay);
    form->addRow("Direct advertised host", host);
    form->addRow("Direct listen port", port);
    auto* saving = new QCheckBox("Save messages received or sent by this client", &dialog);
    saving->setObjectName("communitySaveMessages");
    saving->setChecked(saved.value("messages/enabled", false).toBool());
    auto* path = new QLineEdit(
        saved.value("messages/file", QDir::homePath() + "/disquisition-messages.db").toString(),
        &dialog);
    path->setObjectName("communityMessageFile");
    auto* maximum = new QLineEdit(saved.value("messages/maximum").toString(), &dialog);
    maximum->setPlaceholderText("Unlimited");
    form->addRow(saving);
    form->addRow("SQLite file", path);
    form->addRow("Maximum saved messages", maximum);
    auto* input = new QComboBox(&dialog);
    auto* output = new QComboBox(&dialog);
    input->addItem("System default", "");
    output->addItem("System default", "");
    for (const auto& device : QMediaDevices::audioInputs())
        input->addItem(device.description(), QString::fromUtf8(device.id()));
    for (const auto& device : QMediaDevices::audioOutputs())
        output->addItem(device.description(), QString::fromUtf8(device.id()));
    input->setCurrentIndex(std::max(0, input->findData(saved.value("community/input", ""))));
    output->setCurrentIndex(std::max(0, output->findData(saved.value("community/output", ""))));
    input->setEnabled(voiceCid_.isEmpty());
    output->setEnabled(voiceCid_.isEmpty());
    form->addRow("Microphone", input);
    form->addRow("Audio output", output);
    auto* explanation =
        new QLabel("Community metadata is always saved. Chat history stays on this device; missed "
                   "messages are not downloaded. Disconnect to change connection settings.",
                   &dialog);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        attempt([&] {
            const auto address = relay->text().trimmed();
            community::normalizeRelayEndpoint(address);
            bool validMaximum = false;
            const auto count = maximum->text().toLongLong(&validMaximum);
            if (!maximum->text().trimmed().isEmpty() && (!validMaximum || count <= 0))
                throw std::runtime_error("Maximum messages must be a positive number or blank.");
            if (saving->isChecked() && path->text().trimmed().isEmpty())
                throw std::runtime_error("Choose a message file.");
            std::unique_ptr<chat::RecentMessages> replacement;
            if (saving->isChecked())
                replacement = std::make_unique<chat::RecentMessages>(
                    path->text().trimmed().toStdString(), maximum->text().trimmed().isEmpty()
                                                              ? std::nullopt
                                                              : std::optional<std::int64_t>(count));
            saved.setValue("community/relay", address);
            saved.setValue("community/host", host->text().trimmed());
            saved.setValue("community/port", port->value());
            saved.setValue("community/input", input->currentData());
            saved.setValue("community/output", output->currentData());
            saved.setValue("messages/enabled", saving->isChecked());
            saved.setValue("messages/file", path->text().trimmed());
            saved.setValue("messages/maximum", maximum->text().trimmed());
            history_ = std::move(replacement);
            historyLoaded_.clear();
            loadHistory(cid_, channel_);
            render();
            dialog.accept();
        });
    });
    dialog.resize(620, 430);
    dialog.exec();
}

void CommunityPanel::create() {
    bool ok = false;
    const auto name = QInputDialog::getText(this, "Create community", "Community name",
                                            QLineEdit::Normal, {}, &ok);
    if (ok && !name.trimmed().isEmpty())
        attempt([&] { selectCommunity(node_->createCommunity(name)); });
}

void CommunityPanel::acceptInvite(const QString& link) {
    emit activated();
    if (!online_) {
        pendingLink_ = link;
        note("Enter your name and connect to accept this invite.");
        return;
    }
    attempt([&] {
        node_->joinCommunity(link);
        note("Looking for a community host. Retrying automatically if nobody is reachable.");
    });
}

void CommunityPanel::join() {
    QDialog dialog(this);
    dialog.setWindowTitle("Join a community");
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel("Paste an invite link, or open a QR code image.", &dialog);
    layout->addWidget(explanation);
    auto* link = new QPlainTextEdit(&dialog);
    link->setObjectName("communityInviteInput");
    link->setPlaceholderText("disquisition://join/...");
    layout->addWidget(link);
    auto* qr = button("Open QR image", "communityOpenQr", &dialog);
    layout->addWidget(qr);
    connect(qr, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Open invite QR image", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
        if (path.isEmpty())
            return;
        attempt([&] {
            const auto prepared = prepareImageFile(path);
            if (!prepared.error.isEmpty())
                throw std::runtime_error(prepared.error.toStdString());
            link->setPlainText(community::readInviteQr(QImage::fromData(prepared.bytes)));
        });
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("Join community");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        attempt([&] {
            community::Invite::fromLink(link->toPlainText().trimmed());
            acceptInvite(link->toPlainText().trimmed());
            dialog.accept();
        });
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(580, 250);
    dialog.exec();
}

void CommunityPanel::invite() {
    attempt([&] {
        const auto link = node_->invite(cid_);
        QDialog dialog(this);
        dialog.setWindowTitle("Invite to " + node_->communities()[cid_].name);
        auto* layout = new QVBoxLayout(&dialog);
        auto* code = new QPlainTextEdit(link, &dialog);
        code->setReadOnly(true);
        code->setMaximumHeight(100);
        layout->addWidget(code);
        const auto qr = community::inviteQr(link);
        auto* image = new QLabel(&dialog);
        image->setPixmap(
            QPixmap::fromImage(qr).scaled(360, 360, Qt::KeepAspectRatio, Qt::FastTransformation));
        image->setAlignment(Qt::AlignCenter);
        layout->addWidget(image);
        auto* actions = new QHBoxLayout;
        auto* copy = button("Copy invite link", "communityCopyInvite", &dialog);
        auto* save = button("Save QR image", "communitySaveQr", &dialog);
        actions->addWidget(copy);
        actions->addWidget(save);
        layout->addLayout(actions);
        connect(copy, &QPushButton::clicked, &dialog,
                [link] { QApplication::clipboard()->setText(link); });
        connect(save, &QPushButton::clicked, &dialog, [&] {
            const auto path = QFileDialog::getSaveFileName(&dialog, "Save invite QR",
                                                           "community-invite.png",
                                                           "PNG image (*.png)");
            if (!path.isEmpty() && !qr.save(path))
                note("Could not save QR image.");
        });
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(580, 540);
        dialog.exec();
    });
}

void CommunityPanel::manage() {
    if (!node_ || !node_->communities().contains(cid_))
        return;
    const auto cid = cid_;
    const auto model = node_->communities().value(cid);
    QDialog dialog(this);
    dialog.setWindowTitle("Manage " + model.name);
    dialog.setObjectName("communityManageDialog");
    auto* layout = new QVBoxLayout(&dialog);
    auto* content = new QHBoxLayout;
    auto* categories = new QListWidget(&dialog);
    categories->setObjectName("communityManageCategories");
    categories->setFixedWidth(185);
    categories->setSpacing(2);
    auto* tabs = new QStackedWidget(&dialog);
    tabs->setObjectName("communityManagePages");
    content->addWidget(categories);
    content->addWidget(tabs, 1);
    layout->addLayout(content, 1);
    connect(categories, &QListWidget::currentRowChanged, tabs, &QStackedWidget::setCurrentIndex);
    const auto addPage = [&](QWidget* page, const QString& title) {
        auto* heading = new QLabel(title, page);
        heading->setObjectName("settingsHeading");
        page->layout()->setContentsMargins(20, 12, 12, 12);
        if (auto* form = qobject_cast<QFormLayout*>(page->layout())) {
            form->insertRow(0, heading);
            form->setVerticalSpacing(14);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        }
        else if (auto* vertical = qobject_cast<QVBoxLayout*>(page->layout()))
            vertical->insertWidget(0, heading);
        categories->addItem(title);
        tabs->addWidget(page);
    };
    auto* result = new QLabel(&dialog);
    result->setWordWrap(true);
    const auto apply = [&](const QString& kind, const QJsonObject& data) {
        try {
            node_->edit(cid, kind, data);
            result->setText("Saved. Updates are being distributed to connected members.");
            return true;
        }
        catch (const std::exception& error) {
            result->setText(QString::fromUtf8(error.what()));
            return false;
        }
    };
    auto* general = new QWidget(tabs);
    auto* form = new QFormLayout(general);
    auto* name = new QLineEdit(model.name, general);
    auto* saveName = button("Save name", "communitySaveName", general);
    form->addRow("Community name", name);
    form->addRow(saveName);
    connect(saveName, &QPushButton::clicked, &dialog,
            [&] { apply("rename", {{"name", name->text().trimmed()}}); });
    auto* icon = button("Choose community icon", "communityChooseIcon", general);
    form->addRow(icon);
    connect(icon, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Community icon", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp)");
        if (path.isEmpty())
            return;
        const auto prepared = prepareImageFile(path);
        if (!prepared.error.isEmpty()) {
            result->setText(prepared.error);
            return;
        }
        const auto image = QImage::fromData(prepared.bytes)
                               .scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        apply("icon", {{"bytes", QString::fromLatin1(bytes.toBase64())}});
    });
    auto* ownershipHelp =
        new QLabel("The creator keeps ownership. Admins can make changes independently. "
                   "Any member can host and accept existing valid invites.",
                   general);
    ownershipHelp->setWordWrap(true);
    form->addRow(ownershipHelp);
    addPage(general, "Community");

    auto* channelsPage = new QWidget(tabs);
    auto* channelLayout = new QFormLayout(channelsPage);
    auto* channels = new QComboBox(channelsPage);
    auto* channelName = new QLineEdit(channelsPage);
    auto* topic = new QLineEdit(channelsPage);
    auto* type = new QComboBox(channelsPage);
    type->addItem("Text", "text");
    type->addItem("Voice", "voice");
    const auto fillChannels = [&] {
        channels->clear();
        channels->addItem("New channel", "");
        for (const auto& item : node_->communities()[cid].channels)
            channels->addItem(item.name, item.id);
    };
    fillChannels();
    channelLayout->addRow("Channel", channels);
    channelLayout->addRow("Name", channelName);
    channelLayout->addRow("Type", type);
    channelLayout->addRow("Topic", topic);
    auto* saveChannel = button("Save channel", "communitySaveChannel", channelsPage);
    auto* removeChannel = button("Delete channel", "communityDeleteChannel", channelsPage);
    channelLayout->addRow(saveChannel);
    channelLayout->addRow(removeChannel);
    connect(channels, &QComboBox::currentIndexChanged, &dialog, [&] {
        const auto item = node_->communities()[cid].channels.value(
            channels->currentData().toString());
        channelName->setText(item.name);
        topic->setText(item.topic);
        type->setCurrentIndex(item.type == "voice" ? 1 : 0);
        removeChannel->setEnabled(!item.id.isEmpty());
        type->setEnabled(item.id.isEmpty());
    });
    removeChannel->setEnabled(false);
    connect(saveChannel, &QPushButton::clicked, &dialog, [&] {
        const auto selected = channels->currentData().toString();
        const auto id = selected.isEmpty() ? freshId() : selected;
        if (apply("channel", {{"id", id},
                              {"name", channelName->text().trimmed()},
                              {"topic", topic->text()},
                              {"type", type->currentData().toString()}})) {
            fillChannels();
            channels->setCurrentIndex(channels->findData(id));
        }
    });
    connect(removeChannel, &QPushButton::clicked, &dialog, [&] {
        if (apply("delete-channel", {{"id", channels->currentData().toString()}}))
            fillChannels();
    });
    addPage(channelsPage, "Channels");

    auto* people = new QWidget(tabs);
    auto* peopleForm = new QFormLayout(people);
    auto* member = new QComboBox(people);
    for (const auto& item : model.members)
        member->addItem(item.name + " · " + item.id.left(8), item.id);
    auto* admin = new QCheckBox("Administrator", people);
    auto* saveRole = button("Save role", "communitySaveRole", people);
    auto* remove = button("Remove and block member", "communityRemoveMember", people);
    peopleForm->addRow("Member", member);
    peopleForm->addRow(admin);
    peopleForm->addRow(saveRole);
    peopleForm->addRow(remove);
    const auto memberChanged = [&] {
        const auto peer = member->currentData().toString();
        const auto current = node_->communities()[cid];
        admin->setChecked(current.members.value(peer).admin);
        admin->setEnabled(node_->peerId() == current.owner && peer != current.owner);
        saveRole->setEnabled(admin->isEnabled());
        remove->setEnabled(peer != current.owner);
    };
    connect(member, &QComboBox::currentIndexChanged, &dialog, memberChanged);
    memberChanged();
    connect(saveRole, &QPushButton::clicked, &dialog, [&] {
        apply("admin",
              {{"peer", member->currentData().toString()}, {"enabled", admin->isChecked()}});
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (apply("kick", {{"peer", member->currentData().toString()}})) {
            member->removeItem(member->currentIndex());
        }
    });
    addPage(people, "Members");

    auto* invitesPage = new QWidget(tabs);
    auto* invitesForm = new QFormLayout(invitesPage);
    auto* invites = new QComboBox(invitesPage);
    for (const auto& hash : model.inviteIds())
        invites->addItem(hash.left(16) +
                             (model.revokedInvites.contains(hash) ? " · revoked" : " · active"),
                         hash);
    auto* revoke = button("Revoke invite", "communityRevokeInvite", invitesPage);
    revoke->setEnabled(invites->count() > 0);
    invitesForm->addRow("Invite ID", invites);
    invitesForm->addRow(revoke);
    auto* inviteExplanation = new QLabel(
        "Existing valid invites work while admins are offline. Revoking an invite prevents future "
        "admissions once peers receive the update; it does not remove existing members.",
        invitesPage);
    inviteExplanation->setWordWrap(true);
    invitesForm->addRow(inviteExplanation);
    connect(revoke, &QPushButton::clicked, &dialog, [&] {
        if (apply("revoke", {{"hash", invites->currentData().toString()}}))
            invites->setItemText(invites->currentIndex(),
                                 invites->currentData().toString().left(16) + " · revoked");
    });
    addPage(invitesPage, "Invites");

    auto* hostsPage = new QWidget(tabs);
    auto* hostsLayout = new QVBoxLayout(hostsPage);
    auto* hostHelp =
        new QLabel("Drag members into host priority order. If a member disconnects, the next "
                   "reachable member becomes the preferred host. This does not change permissions.",
                   hostsPage);
    hostHelp->setWordWrap(true);
    hostsLayout->addWidget(hostHelp);
    auto* priority = new QListWidget(hostsPage);
    priority->setDragDropMode(QAbstractItemView::InternalMove);
    auto ordered = model.priority;
    if (!ordered.contains(model.owner))
        ordered.prepend(model.owner);
    for (const auto& peer : model.members.keys())
        if (!ordered.contains(peer))
            ordered.append(peer);
    for (const auto& peer : ordered) {
        if (!model.members.contains(peer))
            continue;
        auto* item = new QListWidgetItem(model.members[peer].name + " · " + peer.left(8), priority);
        item->setData(Qt::UserRole, peer);
    }
    hostsLayout->addWidget(priority);
    auto* savePriority = button("Save host priority", "communitySavePriority", hostsPage);
    hostsLayout->addWidget(savePriority);
    connect(savePriority, &QPushButton::clicked, &dialog, [&] {
        QJsonArray peers;
        for (int i = 0; i < priority->count(); ++i)
            peers.append(priority->item(i)->data(Qt::UserRole).toString());
        apply("priority", {{"peers", peers}});
    });
    addPage(hostsPage, "Host priority");
    layout->addWidget(result);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    categories->setCurrentRow(0);
    dialog.resize(780, 500);
    dialog.exec();
}

void CommunityPanel::selectCommunity(const QString& cid) {
    selectedVoiceCid_.clear();
    selectedVoiceChannel_.clear();
    cid_ = cid;
    channel_.clear();
    if (node_->communities().contains(cid))
        for (const auto& item : orderedChannels(node_->communities()[cid]))
            if (item.type == "text") {
                channel_ = item.id;
                break;
            }
    loadHistory(cid_, channel_);
    refresh();
    render();
}

void CommunityPanel::select(QTreeWidgetItem* item) {
    if (!item)
        return;
    const auto cid = item->data(0, Qt::UserRole).toString();
    const auto channel = item->data(0, Qt::UserRole + 1).toString();
    if (channel.isEmpty()) {
        if (cid != cid_)
            selectCommunity(cid);
        return;
    }
    if (node_->communities().value(cid).channels.value(channel).type == "voice") {
        selectedVoiceCid_ = cid;
        selectedVoiceChannel_ = channel;
        refresh();
        return;
    }
    selectedVoiceCid_.clear();
    selectedVoiceChannel_.clear();
    cid_ = cid;
    channel_ = channel;
    unread_[key(cid_, channel_)] = 0;
    loadHistory(cid_, channel_);
    refresh();
    render();
}

QString CommunityPanel::memberName(const QString& cid, const QString& peer) const {
    if (!node_ || !node_->communities().contains(cid))
        return peer.left(8);
    return node_->communities()[cid].members.value(peer).name;
}

void CommunityPanel::refresh() {
    const bool usable = node_ != nullptr;
    if (usable &&
        !node_->communities().value(selectedVoiceCid_).channels.contains(selectedVoiceChannel_)) {
        selectedVoiceCid_.clear();
        selectedVoiceChannel_.clear();
    }
    QString currentVoiceCid, currentVoiceChannel;
    if (usable && online_) {
        for (const auto& model : node_->communities()) {
            const auto presence = node_->presence(model.id, node_->peerId());
            if (!presence.voiceChannel.isEmpty()) {
                currentVoiceCid = model.id;
                currentVoiceChannel = presence.voiceChannel;
                break;
            }
        }
    }
    if (!voiceCid_.isEmpty() && currentVoiceCid.isEmpty()) {
        leaveVoice();
        return;
    }
    voiceCid_ = currentVoiceCid;
    voiceChannel_ = currentVoiceChannel;
    if (!voiceCid_.isEmpty()) {
        const auto presence = node_->presence(voiceCid_, node_->peerId());
        QSignalBlocker mute(mute_), deafen(deafen_);
        deafen_->setChecked(presence.deafened);
        if (!presence.deafened)
            mute_->setChecked(presence.muted);
    }
    for (auto* action : {invite_, manage_, send_, image_, gif_, voice_, mute_, deafen_, share_})
        action->setEnabled(false);
    composer_->setEnabled(false);
    connect_->setEnabled(usable);
    connect_->setText(online_ ? "Disconnect" : "Connect");
    name_->setEnabled(!online_);
    create_->setEnabled(usable && online_);
    join_->setEnabled(usable && online_);
    voiceControls_->setVisible(online_ && !voiceCid_.isEmpty());
    if (!usable) {
        note(failure_);
        return;
    }
    if (cid_.isEmpty() && !node_->communities().isEmpty()) {
        cid_ = node_->communities().firstKey();
        for (const auto& channel : orderedChannels(node_->communities()[cid_]))
            if (channel.type == "text") {
                channel_ = channel.id;
                break;
            }
        loadHistory(cid_, channel_);
        render();
    }
    QMap<QString, bool> expanded;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const auto* item = tree_->topLevelItem(i);
        expanded[item->data(0, Qt::UserRole).toString()] = item->isExpanded();
    }
    QSignalBlocker block(tree_);
    tree_->clear();
    for (const auto& model : node_->communities()) {
        auto* root = new QTreeWidgetItem(tree_, {model.name});
        root->setData(0, Qt::UserRole, model.id);
        root->setExpanded(expanded.value(model.id, true));
        auto font = root->font(0);
        font.setBold(true);
        root->setFont(0, font);
        if (!model.icon.isEmpty()) {
            ImageBytes image(model.icon);
            if (image.canRead() && image.size().width() <= 512 && image.size().height() <= 512)
                root->setIcon(0, QIcon(QPixmap::fromImage(image.read())));
        }
        for (const auto& channel : orderedChannels(model)) {
            const auto unread = unread_.value(key(model.id, channel.id));
            const auto count =
                channel.type == "voice"
                    ? std::count_if(model.members.begin(), model.members.end(),
                                    [&](const auto& member) {
                                        return node_->online(model.id).contains(member.id) &&
                                               node_->presence(model.id, member.id).voiceChannel ==
                                                   channel.id;
                                    })
                    : 0;
            auto label = (channel.type == "text" ? "# " : "") + channel.name;
            if (count)
                label += "  (" + QString::number(count) + ')';
            if (unread)
                label += "  [" + QString::number(unread) + ']';
            auto* child = new QTreeWidgetItem(root, {label});
            if (channel.type == "voice")
                child->setIcon(0, style()->standardIcon(QStyle::SP_MediaVolume));
            child->setData(0, Qt::UserRole, model.id);
            child->setData(0, Qt::UserRole + 1, channel.id);
            const bool selectedVoice = model.id == selectedVoiceCid_ &&
                                       channel.id == selectedVoiceChannel_;
            const bool activeVoice = model.id == voiceCid_ && channel.id == voiceChannel_;
            if (selectedVoice ||
                (selectedVoiceChannel_.isEmpty() && model.id == cid_ && channel.id == channel_))
                tree_->setCurrentItem(child);
            if (channel.type != "voice")
                continue;
            child->setExpanded(true);
            if (activeVoice)
                child->setForeground(0, QColor("#9dccca"));
            for (const auto& peer : model.members) {
                const auto state = node_->presence(model.id, peer.id);
                if (!node_->online(model.id).contains(peer.id) || state.voiceChannel != channel.id)
                    continue;
                QStringList activity;
                if (state.muted)
                    activity.append("muted");
                if (state.deafened)
                    activity.append("deafened");
                if (state.sharing)
                    activity.append("sharing screen");
                auto label = peer.name;
                if (peer.id == node_->peerId())
                    label += " (you)";
                auto* participant = new QTreeWidgetItem(child, {label});
                participant->setFlags(Qt::ItemIsEnabled);
                participant->setData(0, Qt::UserRole + 2, peer.id);
                participant->setData(0, Qt::UserRole + 3, activity);
                participant->setSizeHint(0, QSize(160, 28));
                auto* row = new QWidget(tree_);
                row->setAccessibleName(label + ", " + activity.join(", "));
                auto* line = new QHBoxLayout(row);
                line->setContentsMargins(0, 0, 4, 0);
                line->setSpacing(5);
                auto* avatar = new QLabel(peer.name.left(1).toUpper(), row);
                avatar->setFixedSize(20, 20);
                avatar->setAlignment(Qt::AlignCenter);
                avatar->setStyleSheet(
                    "background:#39484b; color:#bed9d4; border-radius:10px; font-size:11px;");
                line->addWidget(avatar);
                auto* name = new QLabel(label, row);
                name->setTextFormat(Qt::PlainText);
                name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                name->setToolTip(label);
                line->addWidget(name, 1);
                for (const auto& kind : activity) {
                    auto* indicator = new QLabel(row);
                    indicator->setObjectName("voiceActivity");
                    indicator->setPixmap(voiceIndicator(kind));
                    indicator->setToolTip(kind);
                    indicator->setAccessibleName(kind);
                    indicator->setFixedSize(16, 16);
                    line->addWidget(indicator);
                }
                tree_->setItemWidget(participant, 0, row);
            }
            if (selectedVoice && !activeVoice && online_ && model.isMember(node_->peerId())) {
                auto* action = new QTreeWidgetItem(child);
                action->setFlags(Qt::ItemIsEnabled);
                auto* join = button(voiceCid_.isEmpty() ? "Join voice" : "Switch to this channel",
                                    "communityJoinVoice", tree_);
                join->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                action->setSizeHint(0, QSize(160, 34));
                tree_->setItemWidget(action, 0, join);
                connect(join, &QPushButton::clicked, this, &CommunityPanel::joinVoice);
            }
        }
    }
    const auto model = node_->communities().value(cid_);
    if (!model.id.isEmpty() && !model.channels.contains(channel_)) {
        channel_.clear();
        for (const auto& item : orderedChannels(model))
            if (item.type == "text") {
                channel_ = item.id;
                break;
            }
        if (!channel_.isEmpty()) {
            loadHistory(cid_, channel_);
            render();
        }
    }
    const auto channel = model.channels.value(channel_);
    const bool member = model.isMember(node_->peerId());
    const bool textChannel = member && channel.type == "text";
    heading_->setText(model.id.isEmpty() ? "Your communities"
                                         : model.name + " / " +
                                               (channel.type == "text" ? "#" : "") + channel.name);
    topic_->setText(channel.topic.isEmpty()
                        ? (model.id.isEmpty()
                               ? "Create a community or join with an invite link or QR image."
                               : QString::number(node_->online(cid_).size()) + " online · " +
                                     QString::number(model.members.size()) + " members · host " +
                                     memberName(cid_, node_->host(cid_)))
                        : channel.topic);
    const bool admin = online_ && model.canAdmin(node_->peerId()) && !node_->synchronizing(cid_);
    invite_->setEnabled(admin);
    manage_->setEnabled(admin);
    composer_->setEnabled(online_ && textChannel);
    send_->setEnabled(online_ && textChannel);
    image_->setEnabled(online_ && textChannel);
    gif_->setEnabled(online_ && textChannel);
    members_->clear();
    for (const auto& peer : model.members) {
        const bool present = node_->online(cid_).contains(peer.id);
        QString label = (present ? "+ " : "  ") + peer.name;
        if (peer.id == model.owner)
            label += " · owner";
        else if (peer.admin)
            label += " · admin";
        auto* item = new QListWidgetItem(label, members_);
        item->setToolTip(peer.id + "\n" + peer.endpoint);
        if (!present)
            item->setForeground(QColor("#858c96"));
    }
    const bool inVoice = !voiceCid_.isEmpty();
    voice_->setEnabled(inVoice);
    mute_->setEnabled(inVoice && !deafen_->isChecked());
    deafen_->setEnabled(inVoice);
    share_->setEnabled(inVoice);
    share_->setText(capture_.running() ? "Stop sharing" : "Share screen");
    if (inVoice) {
        const auto voiceModel = node_->communities().value(voiceCid_);
        if (!online_ || !voiceModel.isMember(node_->peerId()) ||
            !voiceModel.channels.contains(voiceChannel_) ||
            voiceModel.channels[voiceChannel_].type != "voice") {
            leaveVoice();
            return;
        }
        voiceStatus_->setText(voiceModel.name + " / " + voiceModel.channels[voiceChannel_].name);
        QSet<QString> active, streaming;
        for (const auto& peer : node_->online(voiceCid_)) {
            const auto state = node_->presence(voiceCid_, peer);
            if (peer != node_->peerId() && state.voiceChannel == voiceChannel_) {
                active.insert(peer);
                if (state.sharing)
                    streaming.insert(peer);
            }
        }
        for (const auto& peer : audioPeers_ - active) {
            audio_.remove(peer);
            watchers_.remove(peer);
        }
        audioPeers_ = active;
        for (const auto& peer : streams_ - streaming) {
            stage_->removeStream(peer);
            watching_.remove(peer);
            decoders_.erase(peer);
            frames_.remove(peer);
        }
        for (const auto& peer : streaming - streams_)
            stage_->addStream(peer, false, memberName(voiceCid_, peer));
        streams_ = streaming;
    }
    else
        voiceStatus_->setText("Voice disconnected");
}

void CommunityPanel::openHistory() {
    history_.reset();
    historyLoaded_.clear();
    QSettings settings;
    if (!settings.value("messages/enabled", false).toBool())
        return;
    attempt([&] {
        const auto maximum = settings.value("messages/maximum").toString();
        const auto limit = maximum.isEmpty() ? std::nullopt
                                             : std::optional<std::int64_t>(maximum.toLongLong());
        history_ = std::make_unique<chat::RecentMessages>(
            settings.value("messages/file", QDir::homePath() + "/disquisition-messages.db")
                .toString()
                .toStdString(),
            limit);
        loadHistory(cid_, channel_);
        render();
    });
}

void CommunityPanel::loadHistory(const QString& cid, const QString& channel) {
    if (!history_ || cid.isEmpty() || channel.isEmpty() ||
        historyLoaded_.contains(key(cid, channel)))
        return;
    attempt([&] {
        const auto loaded = history_->recent(cid.toStdString(), channel.toStdString());
        auto& entries = entries_[key(cid, channel)];
        QSet<QString> existing;
        for (const auto& entry : entries)
            existing.insert(text(entry.message.messageId));
        QList<Entry> previous;
        for (const auto& message : loaded)
            if (!existing.contains(text(message.messageId)))
                previous.append({message, {}, false});
        previous.append(entries);
        entries = std::move(previous);
        historyLoaded_.insert(key(cid, channel));
    });
}

void CommunityPanel::append(const QString& cid, const QString& channel, Entry entry, bool persist) {
    entry.message.communityId = cid.toStdString();
    entry.message.channelId = channel.toStdString();
    if (persist && history_ && entry.image.isEmpty())
        attempt([&] { history_->append(entry.message); });
    auto& list = entries_[key(cid, channel)];
    if (list.isEmpty() && cid == cid_ && channel == channel_) {
        media_->clear();
        transcript_->clear();
    }
    if (!entry.image.isEmpty()) {
        entry.message.messageId = freshId().toStdString();
        cachedImageBytes_ += entry.image.size();
        cachedImages_.append({key(cid, channel), text(entry.message.messageId)});
    }
    list.append(entry);
    if (list.size() > 1000) {
        cachedImageBytes_ -= list.first().image.size();
        list.removeFirst();
    }
    bool rerender = false;
    while (cachedImages_.size() > 128 || cachedImageBytes_ > 24 * 1024 * 1024) {
        const auto oldest = cachedImages_.takeFirst();
        for (auto& cached : entries_[oldest.first]) {
            if (text(cached.message.messageId) != oldest.second || cached.image.isEmpty())
                continue;
            cachedImageBytes_ -= cached.image.size();
            cached.image.clear();
            cached.message.body = "[image no longer cached]";
            if (oldest.first == key(cid_, channel_))
                rerender = true;
            break;
        }
    }
    if (cid == cid_ && channel == channel_)
        showEntry(entry);
    else {
        ++unread_[key(cid, channel)];
        refresh();
    }
    if (rerender)
        render();
}

void CommunityPanel::showEntry(const Entry& entry) {
    const auto when = QDateTime::fromSecsSinceEpoch(entry.message.timestamp).toString("HH:mm");
    const auto body = text(entry.message.body);
    transcript_->append(
        "<p style='margin:4px 0'><span style='color:#858c96'>" + when +
        "</span> <b style='color:#9dccca'>" + text(entry.message.sender).toHtmlEscaped() + "</b> " +
        body.toHtmlEscaped().replace('\n', "<br>") +
        (entry.undelivered
             ? " <span style='color:#d4ae77'>[not delivered: no peers connected]</span>"
             : "") +
        "</p>");
    if (!entry.image.isEmpty())
        media_->appendImage(entry.image);
    if (QSettings().value("gifs/showLinkedMedia", true).toBool())
        for (const auto& link : findMediaLinks(body))
            media_->appendRemote(link.url);
}

void CommunityPanel::render() {
    media_->clear();
    transcript_->clear();
    const auto entries = entries_.value(key(cid_, channel_));
    if (entries.isEmpty()) {
        const bool voice = node_ &&
                           node_->communities().value(cid_).channels.value(channel_).type ==
                               "voice";
        transcript_->setPlainText(
            voice ? "Join this voice channel to talk or share your screen."
                  : (cid_.isEmpty()
                         ? "Your communities live on member devices. Create one to get started."
                         : "No messages on this device yet. Messages are delivered live."));
    }
    else
        for (const auto& entry : entries)
            showEntry(entry);
}

void CommunityPanel::sendChat() {
    if (!online_ || composer_->text().trimmed().isEmpty())
        return;
    attempt([&] {
        const auto body = composer_->text().trimmed();
        const auto id = freshId();
        const auto stamp = QDateTime::currentSecsSinceEpoch();
        const int count =
            node_->sendPacket(cid_, channel_, "chat",
                              {{"id", id}, {"body", body}, {"timestamp", double(stamp * 1000)}});
        if (entries_[key(cid_, channel_)].isEmpty()) {
            media_->clear();
            transcript_->clear();
        }
        append(cid_, channel_,
               {{stamp, name_->text().trimmed().toStdString(), body.toStdString(), "mint",
                 cid_.toStdString(), channel_.toStdString(), id.toStdString()},
                {},
                count == 0});
        composer_->clear();
    });
}

void CommunityPanel::prepareUpload(std::function<PreparedImage()> prepare) {
    const auto cid = cid_, channel = channel_;
    imagePool_.start([this, cid, channel, prepare = std::move(prepare)] {
        PreparedImage image;
        try {
            image = prepare();
        }
        catch (const std::exception& error) {
            image.error = QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, cid, channel, image = std::move(image)] { sendImage(image, cid, channel); },
            Qt::QueuedConnection);
    });
}

void CommunityPanel::sendImage(const PreparedImage& image, const QString& cid,
                               const QString& channel) {
    if (!online_ || !image.error.isEmpty() || image.bytes.isEmpty()) {
        note(image.error);
        return;
    }
    if (!node_->communities().value(cid).isMember(node_->peerId()) ||
        node_->communities().value(cid).channels.value(channel).type != "text")
        return;
    if (uploads_.size() >= 4) {
        note("Wait for the current image uploads to finish.");
        return;
    }
    const auto transfer = QRandomGenerator::global()->generate();
    uploads_.append({cid, channel, transfer, splitImage(image.bytes), 0});
    imageTimer_.start();
    append(cid, channel,
           {{QDateTime::currentSecsSinceEpoch(), name_->text().trimmed().toStdString(), "", "mint"},
            image.bytes,
            node_->online(cid).size() <= 1},
           false);
}

void CommunityPanel::receive(const QString& cid, const QString& channel, const QString& peer,
                             const QString& type, const QJsonObject& data) {
    const auto sender = memberName(cid, peer);
    if (type == "chat") {
        if (cid == cid_ && channel == channel_ && entries_[key(cid, channel)].isEmpty()) {
            media_->clear();
            transcript_->clear();
        }
        append(cid, channel,
               {{qint64(data.value("timestamp").toDouble()) / 1000, sender.toStdString(),
                 data.value("body").toString().toStdString(), "mint", cid.toStdString(),
                 channel.toStdString(), data.value("id").toString().toStdString()},
                {},
                false});
    }
    else if (type == "audio" && cid == voiceCid_ && channel == voiceChannel_) {
        audio_.receive(peer, QByteArray::fromBase64(data.value("bytes").toString().toLatin1()));
    }
    else if (type == "image") {
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        const auto transfer = data.value("transfer").toDouble();
        const auto index = data.value("index").toInt(-1), count = data.value("count").toInt();
        if (transfer < 0 || transfer > UINT32_MAX || transfer != quint32(transfer) || index < 0 ||
            count <= 0 || count > int(chat::kMaxImageChunks) || index >= count ||
            bytes.size() > qsizetype(chat::kImageChunkSize))
            return;
        const auto image = images_.add(key(cid, channel) + '/' + peer, quint32(transfer), index,
                                       count, bytes, QDateTime::currentMSecsSinceEpoch());
        if (image && isDisplayableImage(*image))
            append(cid, channel,
                   {{QDateTime::currentSecsSinceEpoch(), sender.toStdString(), "", "mint"},
                    *image,
                    false},
                   false);
    }
    else if (type == "watch") {
        if (data.value("enabled").toBool() && capture_.running())
            watchers_.insert(peer);
        else
            watchers_.remove(peer);
    }
    else if (type == "screen" && watching_.contains(peer) && cid == voiceCid_ &&
             channel == voiceChannel_) {
        const auto frame = data.value("frame").toDouble(-1);
        const auto index = data.value("index").toInt(-1), count = data.value("count").toInt();
        const auto chunk = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        if (frame < 0 || frame > UINT32_MAX || frame != quint32(frame) || index < 0 || count <= 0 ||
            count > kMaxScreenChunks || index >= count || chunk.size() > kScreenChunkBytes)
            return;
        QByteArray encoded;
        if (frames_.add(peer, quint32(frame), index, count, chunk, encoded)) {
            const auto image = decoders_[peer].decode(quint32(frame),
                                                      data.value("keyframe").toBool(), encoded);
            if (!image.isNull())
                stage_->showFrame(peer, image, encoded.size());
        }
    }
}

void CommunityPanel::joinVoice() {
    if (!online_)
        return;
    const auto cid = selectedVoiceCid_, selectedChannel = selectedVoiceChannel_;
    if (node_->communities().value(cid).channels.value(selectedChannel).type != "voice")
        return;
    requestMicrophoneAccess(this, [this, cid, selectedChannel](bool granted) {
        if (!online_)
            return;
        if (!granted) {
            note("Microphone access is required to join voice.");
            return;
        }
        const auto model = node_->communities().value(cid);
        if (!model.isMember(node_->peerId()) ||
            model.channels.value(selectedChannel).type != "voice")
            return;
        leaveVoice();
        QSettings settings;
        if (!audio_.start(settings.value("community/input").toString(),
                          settings.value("community/output").toString())) {
            note("The selected audio devices are unavailable.");
            return;
        }
        voiceCid_ = cid;
        voiceChannel_ = selectedChannel;
        voiceState();
        refresh();
    });
}

void CommunityPanel::voiceState() {
    if (voiceCid_.isEmpty() || !node_)
        return;
    audio_.setMuted(mute_->isChecked() || deafen_->isChecked());
    audio_.setDeafened(deafen_->isChecked());
    attempt([&] {
        node_->setVoice(voiceCid_, voiceChannel_, mute_->isChecked() || deafen_->isChecked(),
                        deafen_->isChecked(), capture_.running());
    });
    refresh();
}

void CommunityPanel::leaveVoice() {
    const auto previous = voiceCid_;
    voiceCid_.clear();
    voiceChannel_.clear();
    stopSharing();
    if (node_ && online_ && !previous.isEmpty())
        attempt([&] { node_->setVoice(previous, {}); });
    audio_.stop();
    audioPeers_.clear();
    watchers_.clear();
    for (const auto& peer : streams_)
        stage_->removeStream(peer);
    streams_.clear();
    watching_.clear();
    decoders_.clear();
    frames_ = {};
    {
        QSignalBlocker a(mute_), b(deafen_);
        mute_->setChecked(false);
        deafen_->setChecked(false);
    }
    refresh();
}

void CommunityPanel::share() {
    if (capture_.running()) {
        stopSharing();
        return;
    }
    if (voiceCid_.isEmpty() || !requestScreenRecordingAccess())
        return;
    SharePicker dialog(this);
    QPointer<SharePicker> guarded(&dialog);
    listShareSources([guarded](QList<ShareSource> sources) {
        if (guarded)
            guarded->setSources(sources);
    });
    if (dialog.exec() != QDialog::Accepted || !dialog.chosen())
        return;
    if (!capture_.start(*dialog.chosen())) {
        note("Could not start screen sharing.");
        return;
    }
    stage_->addStream(node_->peerId(), true, name_->text().trimmed());
    voiceState();
}

void CommunityPanel::stopSharing() {
    const bool wasSharing = capture_.running();
    capture_.stop();
    watchers_.clear();
    if (node_) {
        stage_->removeStream(node_->peerId());
        decoders_.erase(node_->peerId());
    }
    if (wasSharing)
        voiceState();
}

void CommunityPanel::watch(const QString& peer, bool enabled) {
    if (voiceCid_.isEmpty())
        return;
    attempt([&] {
        node_->sendPacket(voiceCid_, voiceChannel_, "watch",
                          {{"target", peer}, {"enabled", enabled}});
        if (enabled)
            watching_.insert(peer);
        else {
            watching_.remove(peer);
            decoders_.erase(peer);
        }
        stage_->setWatching(peer, enabled);
    });
}

void CommunityPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (online_ && composer_->isEnabled() &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
        event->acceptProposedAction();
}
void CommunityPanel::dropEvent(QDropEvent* event) {
    if (!online_ || !composer_->isEnabled())
        return;
    if (event->mimeData()->hasImage()) {
        const auto image = qvariant_cast<QImage>(event->mimeData()->imageData());
        prepareUpload([image] { return prepareImage(image); });
    }
    else {
        int count = 0;
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile() && count++ < 4) {
                const auto path = url.toLocalFile();
                prepareUpload([path] { return prepareImageFile(path); });
            }
    }
    event->acceptProposedAction();
}

bool CommunityPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qApp && event->type() == QEvent::FileOpen) {
        const auto url = static_cast<QFileOpenEvent*>(event)->url();
        if (url.scheme() == "disquisition") {
            acceptInvite(url.toString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
