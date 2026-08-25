#pragma once

#include <memory>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace ravo
{

class StudioPresenter;

class StudioCommandController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap ids READ ids CONSTANT)
    Q_PROPERTY(QVariantList paletteEntries READ paletteEntries NOTIFY commandsChanged)
    Q_PROPERTY(QVariantList shortcutEntries READ shortcutEntries NOTIFY commandsChanged)
    Q_PROPERTY(QString paletteQuery READ paletteQuery WRITE setPaletteQuery NOTIFY paletteQueryChanged)
    Q_PROPERTY(bool paletteOpen READ paletteOpen WRITE setPaletteOpen NOTIFY paletteOpenChanged)
    Q_PROPERTY(bool textInputActive READ textInputActive WRITE setTextInputActive NOTIFY commandsChanged)
    Q_PROPERTY(bool settingsOpen READ settingsOpen WRITE setSettingsOpen NOTIFY commandsChanged)
    Q_PROPERTY(bool modalOpen READ modalOpen WRITE setModalOpen NOTIFY commandsChanged)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY commandsChanged)

public:
    explicit StudioCommandController(StudioPresenter &presenter, QObject *parent = nullptr);
    ~StudioCommandController() override;

    [[nodiscard]] QVariantMap ids() const;
    [[nodiscard]] QVariantList paletteEntries() const;
    [[nodiscard]] QVariantList shortcutEntries() const;
    [[nodiscard]] QString paletteQuery() const;
    [[nodiscard]] bool paletteOpen() const noexcept;
    [[nodiscard]] bool textInputActive() const noexcept;
    [[nodiscard]] bool settingsOpen() const noexcept;
    [[nodiscard]] bool modalOpen() const noexcept;
    [[nodiscard]] qulonglong revision() const noexcept;

    void setPaletteQuery(const QString &query);
    void setPaletteOpen(bool open);
    void setTextInputActive(bool active);
    void setSettingsOpen(bool open);
    void setModalOpen(bool open);

    Q_INVOKABLE QVariantList menuEntries(const QString &path) const;
    Q_INVOKABLE QVariantMap action(const QString &action_id) const;
    Q_INVOKABLE QVariantMap executeAction(const QString &action_id,
                                          const QString &source = QStringLiteral("control"));
    Q_INVOKABLE QVariantMap executeCommand(
        const QString &command_id, const QVariant &argument = QVariant(),
        const QString &source = QStringLiteral("control"));
    Q_INVOKABLE void cancelPendingConfirmation(const QString &token);

    [[nodiscard]] static QStringList validateBuiltinDefinitions();
    [[nodiscard]] static int fuzzyScore(const QString &title, const QString &category,
                                        const QStringList &keywords, const QString &command_id,
                                        const QString &query);
    [[nodiscard]] static QString paletteShortcutForPlatform(const QString &platform);

signals:
    void commandsChanged();
    void paletteQueryChanged();
    void paletteOpenChanged();
    void presentationCommandRequested(const QString &id, const QVariant &argument);
    void dispatchRejected(const QString &id, const QString &code, const QString &message);

private:
    struct Impl;

    [[nodiscard]] QVariantMap executeCommandInternal(const QString &command_id,
                                                     const QVariant &argument,
                                                     const QString &source);
    void refresh();

    StudioPresenter &presenter_;
    std::unique_ptr<Impl> impl_;
    QString palette_query_;
    bool palette_open_ = false;
    bool text_input_active_ = false;
    bool settings_open_ = false;
    bool modal_open_ = false;
    qulonglong revision_ = 0;
};

} // namespace ravo
