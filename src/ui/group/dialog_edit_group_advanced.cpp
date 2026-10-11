#include "include/ui/group/dialog_edit_group_advanced.h"

#include "include/configs/common/utils.h"
#include "include/configs/generate.h"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/DatabaseManager.h"
#include "include/database/IpListsRepo.h"
#include "include/database/SettingsRepo.h"
#include "include/global/GuiUtils.hpp"

#include <QGuiApplication>
#include <QHostAddress>
#include <QRegularExpression>
#include <QScreen>
#include <QSslSocket>
#include <QStandardItemModel>
#include <QStyle>

namespace {
    // Index 0 of an override combo is Keep Default; the enum values follow.
    template <typename E>
    int overrideIndex(const std::optional<E> &value) {
        return value ? static_cast<int>(*value) + 1 : 0;
    }

    template <typename E>
    std::optional<E> overrideAt(int index) {
        if (index <= 0) return std::nullopt;
        return static_cast<E>(index - 1);
    }

    // Empty unless the text is a bare IP address or host name: no scheme, port or path.
    QString groupEndpointHost(QString text) {
        text = text.trimmed();
        if (text.startsWith('[') && text.endsWith(']')) text = text.mid(1, text.size() - 2);
        if (IsIpAddressV6(text)) return QHostAddress(text).toString();
        // Qt reads "1.2.3" as IPv4 and "999.1.1.1" would pass as a name; both are typos, so neither is accepted.
        if (IsIpAddressV4(text)) return QHostAddress(text).toString() == text ? text : QString();
        static const QRegularExpression hostname(QStringLiteral(
            R"(^(?:(?!-)[A-Za-z0-9_-]{1,63}(?<!-)\.)*(?![0-9]+$)(?!-)[A-Za-z0-9_-]{1,63}(?<!-)$)"));
        const auto ace = Configs::toAceHost(text);
        return ace.size() <= 253 && hostname.match(ace).hasMatch() ? ace : QString();
    }
}

DialogEditGroupAdvanced::DialogEditGroupAdvanced(const Configs::SubscriptionOptions &options, int serverIntervalHours,
                                                 const Configs::EndpointSource &endpoint, bool subscription, QWidget *parent)
    : QDialog(parent), ui(new Ui::DialogEditGroupAdvanced), options(options), endpoint(endpoint) {
    ui->setupUi(this);
    loadEndpoint();
    if (!subscription) {
        ui->request_box->hide();
        ui->update_box->hide();
    }

    const auto defaults = Subscription::ResolveIdentity(nullptr);
    globalSendHwid = defaults.sendHwid;
    ui->send_hwid->setItemText(static_cast<int>(Configs::sendHwid::keepDefault),
                               tr("Keep Default (%1)").arg(globalSendHwid ? tr("On") : tr("Off")));
    ui->user_agent->setPlaceholderText(defaults.userAgent);
    ui->hwid->setPlaceholderText(defaults.device.hwid);
    ui->hwid_os->setPlaceholderText(defaults.device.os);
    ui->hwid_os_version->setPlaceholderText(defaults.device.osVersion);
    ui->hwid_model->setPlaceholderText(defaults.device.model);

    ui->user_agent->setText(options.user_agent);
    ui->send_hwid->setCurrentIndex(static_cast<int>(options.send_hwid));
    ui->hwid->setText(options.hwid);
    ui->hwid_os->setText(options.hwid_os);
    ui->hwid_os_version->setText(options.hwid_os_version);
    ui->hwid_model->setText(options.hwid_model);
    ui->keep_working->setChecked(options.keep_working);
    ui->remove_duplicates->setChecked(options.remove_duplicates);
    ui->remove_insecure->setChecked(options.remove_insecure);
    ui->remove_invalid->setChecked(options.remove_invalid);
    ui->url_test->setChecked(options.url_test);
    ui->remove_unavailable->setChecked(options.remove_unavailable);
    ui->sort_by_latency->setChecked(options.sort_by_latency);

    connect(ui->send_hwid, &QComboBox::currentIndexChanged, this, [this] { syncHwidFields(); });
    syncHwidFields();

    const auto *checkStyle = ui->url_test->style();
    ui->url_test_follow_ups->setContentsMargins(checkStyle->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, ui->url_test)
                                                    + checkStyle->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, ui->url_test),
                                                0, 0, 0);
    connect(ui->url_test, &QCheckBox::toggled, this, [this] { syncUrlTestFollowUps(); });
    syncUrlTestFollowUps();

    ADD_ASTERISK(this)

    // adjustSize() clamps to 2/3 of the screen.
    const auto *scr = screen() != nullptr ? screen() : QGuiApplication::primaryScreen();
    if (scr != nullptr) resize(sizeHint().expandedTo(QSize(400, 0)).boundedTo(scr->availableGeometry().size()));
}

DialogEditGroupAdvanced::~DialogEditGroupAdvanced() {
    delete ui;
}

void DialogEditGroupAdvanced::loadEndpoint() {
    using Mode = Configs::EndpointSource::Mode;
    ui->endpoint_mode->addItem(tr("Profiles' own addresses"), static_cast<int>(Mode::Inherit));
    ui->endpoint_mode->addItem(tr("Fixed address"), static_cast<int>(Mode::Address));
    ui->endpoint_mode->addItem(tr("IP list"), static_cast<int>(Mode::IpList));

    if (endpoint.mode == Mode::Address) ui->endpoint_address->setText(endpoint.address);
    for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
        ui->endpoint_list->addItem(list->name, list->id);
    }
    if (endpoint.mode == Mode::IpList) {
        // A gone list stays selected, so saving never points the group at another list by itself.
        if (ui->endpoint_list->findData(endpoint.ipListId) < 0) ui->endpoint_list->insertItem(0, tr("Missing list"), endpoint.ipListId);
        ui->endpoint_list->setCurrentIndex(ui->endpoint_list->findData(endpoint.ipListId));
    }
    if (auto *model = qobject_cast<QStandardItemModel *>(ui->endpoint_mode->model()); model != nullptr && ui->endpoint_list->count() == 0) {
        model->item(ui->endpoint_mode->findData(static_cast<int>(Mode::IpList)))->setEnabled(false);
    }
    ui->endpoint_mode->setCurrentIndex(qMax(0, ui->endpoint_mode->findData(static_cast<int>(endpoint.mode))));

    connect(ui->endpoint_mode, &QComboBox::currentIndexChanged, this, [this] { syncEndpoint(); });
    connect(ui->endpoint_list, &QComboBox::currentIndexChanged, this, [this] { syncEndpoint(); });
    syncEndpoint();
}

Configs::EndpointSource::Mode DialogEditGroupAdvanced::endpointMode() const {
    return static_cast<Configs::EndpointSource::Mode>(ui->endpoint_mode->currentData().toInt());
}

void DialogEditGroupAdvanced::syncEndpoint() {
    using Mode = Configs::EndpointSource::Mode;
    const auto mode = endpointMode();
    ui->endpoint_address->setVisible(mode == Mode::Address);
    ui->endpoint_list->setVisible(mode == Mode::IpList);

    QString hint;
    if (mode == Mode::IpList) {
        Configs::EndpointSource source;
        source.mode = mode;
        source.ipListId = ui->endpoint_list->currentData().toInt();
        if (const auto resolution = Configs::ResolveEndpointSource(source); !resolution.address.isEmpty()) {
            hint = tr("Profiles connect to %1 (%2).").arg(resolution.address, resolution.origin);
        } else if (!resolution.problem.isEmpty()) {
            hint = tr("%1, so profiles use their own addresses.").arg(resolution.problem);
        }
    }
    ui->endpoint_hint->setText(hint);
    ui->endpoint_hint->setHidden(hint.isEmpty());
}

void DialogEditGroupAdvanced::syncHwidFields() {
    const auto mode = static_cast<Configs::sendHwid>(ui->send_hwid->currentIndex());
    const bool sent = mode == Configs::sendHwid::on || (mode == Configs::sendHwid::keepDefault && globalSendHwid);
    const QList<QWidget *> fields{ui->hwid_l, ui->hwid, ui->hwid_os_l, ui->hwid_os,
                                  ui->hwid_os_version_l, ui->hwid_os_version, ui->hwid_model_l, ui->hwid_model};
    for (auto *field : fields) field->setEnabled(sent);
}

void DialogEditGroupAdvanced::syncUrlTestFollowUps() {
    const bool tested = ui->url_test->isChecked();
    ui->remove_unavailable->setEnabled(tested);
    ui->sort_by_latency->setEnabled(tested);
}

void DialogEditGroupAdvanced::accept() {
    using Mode = Configs::EndpointSource::Mode;
    Configs::EndpointSource source;
    source.mode = endpointMode();
    if (source.mode == Mode::Address) {
        source.address = groupEndpointHost(ui->endpoint_address->text());
        if (source.address.isEmpty()) {
            MessageBoxWarning(tr("Fixed address"), tr("Enter a host name or an IP address, without a port."));
            ui->endpoint_address->setFocus();
            return;
        }
    } else if (source.mode == Mode::IpList) {
        source.ipListId = ui->endpoint_list->currentData().toInt();
    }
    endpoint = source;

    options.user_agent = ui->user_agent->text().trimmed();
    options.send_hwid = static_cast<Configs::sendHwid>(ui->send_hwid->currentIndex());
    options.hwid = ui->hwid->text().trimmed();
    options.hwid_os = ui->hwid_os->text().trimmed();
    options.hwid_os_version = ui->hwid_os_version->text().trimmed();
    options.hwid_model = ui->hwid_model->text().trimmed();
    options.keep_working = ui->keep_working->isChecked();
    options.remove_duplicates = ui->remove_duplicates->isChecked();
    options.remove_insecure = ui->remove_insecure->isChecked();
    options.remove_invalid = ui->remove_invalid->isChecked();
    options.url_test = ui->url_test->isChecked();
    options.remove_unavailable = ui->remove_unavailable->isChecked();
    options.sort_by_latency = ui->sort_by_latency->isChecked();
    QDialog::accept();
}
