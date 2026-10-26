#include "desktop/mention_completer.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QLineEdit>
#include <QStringListModel>

#include "desktop/mentions.h"

MentionCompleter::MentionCompleter(QLineEdit* editor) : QObject(editor), editor_(editor) {
    model_ = new QStringListModel(this);
    completer_ = new QCompleter(model_, this);
    completer_->setWidget(editor_);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setFilterMode(Qt::MatchStartsWith);
    completer_->setMaxVisibleItems(8);
    completer_->popup()->setObjectName("mentionSuggestions");
    // The completer filters its popup's keys as well. Filters run newest
    // first, so this one sees Tab and Enter before the completer hands them
    // to the editor, which would take them as a focus change and a send.
    completer_->popup()->installEventFilter(this);
    connect(completer_, qOverload<const QString&>(&QCompleter::activated), this,
            &MentionCompleter::insert);
    connect(editor_, &QLineEdit::textEdited, this, &MentionCompleter::update);
}

void MentionCompleter::setMembers(const QStringList& members) {
    QStringList names = members;
    names.append({QStringLiteral("everyone"), QStringLiteral("here")});
    if (names != model_->stringList()) model_->setStringList(names);
}

QAbstractItemView* MentionCompleter::popup() const {
    return completer_->popup();
}

bool MentionCompleter::eventFilter(QObject* watched, QEvent* event) {
    if (watched == completer_->popup() && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Return || key == Qt::Key_Enter) {
            const QModelIndex current = completer_->popup()->currentIndex();
            completer_->popup()->hide();
            if (current.isValid()) insert(current.data().toString());
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

void MentionCompleter::update() {
    QAbstractItemView* popup = completer_->popup();
    const MentionQuery query = mentionQueryAt(editor_->text(), editor_->cursorPosition());
    if (query.start < 0) {
        popup->hide();
        return;
    }
    completer_->setCompletionPrefix(query.prefix);
    if (completer_->completionCount() == 0) {
        popup->hide();
        return;
    }
    completer_->complete();
    // Tab or Enter takes the first suggestion
    popup->setCurrentIndex(completer_->completionModel()->index(0, 0));
}

void MentionCompleter::insert(const QString& name) {
    const QString text = editor_->text();
    const int cursor = editor_->cursorPosition();
    const MentionQuery query = mentionQueryAt(text, cursor);
    if (query.start < 0) return;
    // Completing "@ma|tt" must not leave "tt" behind.
    qsizetype end = cursor;
    while (end < text.size() && !text.at(end).isSpace()) ++end;
    editor_->setSelection(static_cast<int>(query.start), static_cast<int>(end - query.start));
    editor_->insert("@" + name + " ");
}
