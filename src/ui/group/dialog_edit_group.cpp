#include "include/ui/group/dialog_edit_group.h"

#include "include/ui/mainwindow_interface.h"

#include <QClipboard>
#include <QHash>
#include <QStringListModel>
#include <QCompleter>
#include <QTimer>
#include <QAbstractItemView>

#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/ui/group/dialog_edit_group_advanced.h"


#define ADJUST_SIZE runOnThread([=,this] { adjustSize(); adjustPosition(mainwindow); }, this);

DialogEditGroup::DialogEditGroup(const std::shared_ptr<Configs::Group> &ent, QWidget *parent) : QDialog(parent), ui(new Ui::DialogEditGroup) {
    ui->setupUi(this);
    this->ent = ent;

    connect(ui->type, &QComboBox::currentIndexChanged, this, [=,this](int index) {
        ui->cat_sub->setHidden(index == 0);
        ADJUST_SIZE
    });

    ui->name->setText(ent->name);
    const bool globalAutoClear = (Configs::dataManager != nullptr && Configs::dataManager->settingsRepo != nullptr)
        ? Configs::dataManager->settingsRepo->auto_clear_unavailable
        : true;
    ui->auto_clear_unavailable->setTristate(true);
    auto updateAutoClearText = [=, this](Qt::CheckState state) {
        if (state == Qt::PartiallyChecked) {
            ui->auto_clear_unavailable->setText(tr("自动清理不可用节点 (跟随全局: %1)").arg(globalAutoClear ? tr("开启") : tr("关闭")));
        } else if (state == Qt::Checked) {
            ui->auto_clear_unavailable->setText(tr("自动清理不可用节点 (独立强制开启)"));
        } else {
            ui->auto_clear_unavailable->setText(tr("自动清理不可用节点 (独立强制关闭)"));
        }
    };
    if (ent->auto_clear_mode == 1) {
        ui->auto_clear_unavailable->setCheckState(Qt::Checked);
    } else if (ent->auto_clear_mode == 2) {
        ui->auto_clear_unavailable->setCheckState(Qt::Unchecked);
    } else {
        ui->auto_clear_unavailable->setCheckState(Qt::PartiallyChecked);
    }
    updateAutoClearText(ui->auto_clear_unavailable->checkState());
    connect(ui->auto_clear_unavailable, &QCheckBox::checkStateChanged, this, [=](Qt::CheckState state) {
        updateAutoClearText(state);
    });

    const bool isDefaultGroup = (ent->id == 0 || ent->name.compare(tr("Default"), Qt::CaseInsensitive) == 0 || ent->name.compare("Default", Qt::CaseInsensitive) == 0);
    ui->show_all_nodes->setVisible(isDefaultGroup);
    if (isDefaultGroup && Configs::dataManager != nullptr && Configs::dataManager->settingsRepo != nullptr) {
        auto updateShowAllText = [this](bool checked) {
            ui->show_all_nodes->setText(checked ? tr("是否显示全部节点: 是 (更新/新增订阅时添加其节点)")
                                               : tr("是否显示全部节点: 否 (仅显示Default自身内容)"));
        };
        ui->show_all_nodes->setChecked(Configs::dataManager->settingsRepo->default_group_include_all);
        updateShowAllText(ui->show_all_nodes->isChecked());
        connect(ui->show_all_nodes, &QCheckBox::toggled, this, updateShowAllText);
    }

    ui->skip_auto_update->setChecked(ent->skip_auto_update);
    subOptions = ent->sub_options;
    connect(ui->advanced, &QPushButton::clicked, this, [this] {
        auto dialog = new DialogEditGroupAdvanced(subOptions, this);
        connect(dialog, &QDialog::accepted, this, [this, dialog] { subOptions = dialog->Options(); });
        connect(dialog, &QDialog::finished, dialog, &QDialog::deleteLater);
        dialog->open();
    });
    ui->url->setText(ent->url);
    ui->type->setCurrentIndex(ent->url.isEmpty() ? 0 : 1);
    ui->type->currentIndexChanged(ui->type->currentIndex());
    ui->cat_share->setVisible(false);
    if (Configs::dataManager->profilesRepo->GetProfile(ent->front_proxy_id) == nullptr) {
        ent->front_proxy_id = -1;
        Configs::dataManager->groupsRepo->Save(ent);
    }
    if (Configs::dataManager->profilesRepo->GetProfile(ent->landing_proxy_id) == nullptr) {
        ent->landing_proxy_id = -1;
        Configs::dataManager->groupsRepo->Save(ent);
    }
    CACHE.front_proxy = ent->front_proxy_id;
    LANDING.landing_proxy = ent->landing_proxy_id;

    if (ent->id >= 0) { // already a group
        ui->type->setDisabled(true);
        if (!ent->Profiles().isEmpty()) {
            ui->cat_share->setVisible(true);
        }
    }

    auto proxyListRaw = Configs::dataManager->profilesRepo->GetAllProfileIDNameMapped();
    QHash<int, QString> idToName;
    idToName.reserve(proxyListRaw.size());
    for (const auto& [id, name] : proxyListRaw) idToName.insert(id, name);
    QList<std::pair<int, QString>> proxyList;
    // An auto selector moves server on its own, so it cannot hold a fixed front/landing slot.
    const auto selectorIDsRaw = Configs::dataManager->profilesRepo->GetProfileIdsByType("autoselector");
    const QSet<int> selectorIDs(selectorIDsRaw.begin(), selectorIDsRaw.end());
    auto groupIDs = Configs::dataManager->groupsRepo->GetGroupsTabOrder();
    for (auto groupID: groupIDs) {
        auto group = Configs::dataManager->groupsRepo->GetGroup(groupID);
        if (!group) continue;
        const QString prefix = "[" + group->name + "] ";
        for (int profileID : group->profiles) {
            if (selectorIDs.contains(profileID)) continue;
            auto it = idToName.constFind(profileID);
            if (it == idToName.constEnd()) continue;
            proxyList << std::make_pair(profileID, prefix + it.value());
        }
    }
    QStringList proxyNameList;
    proxyNameToId.clear();
    proxyNameToId.insert("None", -1);
    ui->front_proxy->setMaxCount(200);
    ui->landing_proxy->setMaxCount(200);
    ui->front_proxy->blockSignals(true);
    ui->landing_proxy->blockSignals(true);
    ui->front_proxy->setUpdatesEnabled(false);
    ui->landing_proxy->setUpdatesEnabled(false);
    ui->front_proxy->addItem("None", QVariant(-1));
    ui->landing_proxy->addItem("None", QVariant(-1));
    const int comboCap = ui->front_proxy->maxCount();
    int comboCount = 1; // "None" already added
    for (const auto&[id, name] : proxyList) {
        proxyNameList.append(name);
        if (!proxyNameToId.contains(name)) {
            proxyNameToId.insert(name, id);
        }
        if (comboCount < comboCap) {
            ui->front_proxy->addItem(name, QVariant(id));
            ui->landing_proxy->addItem(name, QVariant(id));
            ++comboCount;
        }
    }
    ui->front_proxy->blockSignals(false);
    ui->landing_proxy->blockSignals(false);
    ui->front_proxy->setUpdatesEnabled(true);
    ui->landing_proxy->setUpdatesEnabled(true);

    auto proxyCompleterModel = new QStringListModel(proxyNameList, this);

    auto attachDebouncedCompleter = [this](QComboBox* combo, QCompleter* completer) {
        auto* lineEdit = combo->lineEdit();
        completer->setWidget(lineEdit);
        auto* debounce = new QTimer(this);
        debounce->setSingleShot(true);
        debounce->setInterval(300);
        connect(debounce, &QTimer::timeout, completer, [completer, lineEdit] {
            const QString text = lineEdit->text();
            if (text.isEmpty()) return;
            completer->setCompletionPrefix(text);
            completer->complete();
        });
        connect(lineEdit, &QLineEdit::textEdited, debounce, [debounce, completer](const QString& text) {
            if (text.isEmpty()) {
                debounce->stop();
                completer->popup()->hide();
                return;
            }
            debounce->start();
        });
        connect(completer, qOverload<const QString&>(&QCompleter::activated),
                         lineEdit, [combo, lineEdit](const QString& text) {
            lineEdit->setText(text);
            const int index = combo->findText(text, Qt::MatchExactly);
            if (index >= 0) {
                combo->setCurrentIndex(index);
            }
        });
    };

    ui->front_proxy->setEditable(true);
    ui->front_proxy->setCurrentText(get_proxy_name(CACHE.front_proxy));
    ui->front_proxy->setInsertPolicy(QComboBox::NoInsert);
    auto frontCompleter = new QCompleter(this);
    frontCompleter->setModel(proxyCompleterModel);
    frontCompleter->setCompletionMode(QCompleter::PopupCompletion);
    frontCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    frontCompleter->setFilterMode(Qt::MatchContains);
    ui->front_proxy->setCompleter(nullptr);
    attachDebouncedCompleter(ui->front_proxy, frontCompleter);
    connect(ui->front_proxy, &QComboBox::currentIndexChanged, this, [=,this](int index){
        CACHE.front_proxy = ui->front_proxy->itemData(index).value<int>();
    });

    ui->landing_proxy->setEditable(true);
    ui->landing_proxy->setCurrentText(get_proxy_name(LANDING.landing_proxy));
    ui->landing_proxy->setInsertPolicy(QComboBox::NoInsert);
    auto landingCompleter = new QCompleter(this);
    landingCompleter->setModel(proxyCompleterModel);
    landingCompleter->setCompletionMode(QCompleter::PopupCompletion);
    landingCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    landingCompleter->setFilterMode(Qt::MatchContains);
    ui->landing_proxy->setCompleter(nullptr);
    attachDebouncedCompleter(ui->landing_proxy, landingCompleter);
    connect(ui->landing_proxy, &QComboBox::currentIndexChanged, this, [=,this](int index){
        LANDING.landing_proxy = ui->landing_proxy->itemData(index).value<int>();
    });

    connect(ui->copy_links, &QPushButton::clicked, this, [=] {
        QStringList links;
        auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(ent->Profiles());
        for (const auto &profile: profiles) {
            links += profile->outbound->ExportToLink();
        }
        QApplication::clipboard()->setText(links.join("\n"));
        MessageBoxInfo(software_name, tr("Copied"));
    });
    connect(ui->copy_links_nkr, &QPushButton::clicked, this, [=] {
        QStringList links;
        auto profiles = Configs::dataManager->profilesRepo->GetProfileBatch(ent->Profiles());
        for (const auto &profile: profiles) {
            links += profile->outbound->ExportJsonLink();
        }
        QApplication::clipboard()->setText(links.join("\n"));
        MessageBoxInfo(software_name, tr("Copied"));
    });

    ui->name->setFocus();

    ADJUST_SIZE
}

DialogEditGroup::~DialogEditGroup() {
    delete ui;
}

int DialogEditGroup::resolve_proxy_selection(QComboBox *combo, int fallback) const {
    const QString text = combo->currentText().trimmed();
    if (text.isEmpty() || text == "None") {
        return -1;
    }

    const int index = combo->findText(text, Qt::MatchExactly);
    if (index >= 0) {
        return combo->itemData(index).value<int>();
    }

    auto it = proxyNameToId.constFind(text);
    if (it != proxyNameToId.constEnd()) {
        return it.value();
    }

    return fallback;
}

void DialogEditGroup::accept() {
    if (ent->id >= 0) { // already a group
        if (!ent->url.isEmpty() && ui->url->text().isEmpty()) {
            MessageBoxWarning(tr("Warning"), tr("Please input URL"));
            return;
        }
    }
    ent->name = ui->name->text().trimmed();
    if (ui->auto_clear_unavailable->checkState() == Qt::Checked) {
        ent->auto_clear_mode = 1;
    } else if (ui->auto_clear_unavailable->checkState() == Qt::Unchecked) {
        ent->auto_clear_mode = 2;
    } else {
        ent->auto_clear_mode = 0;
    }
    ent->auto_clear_unavailable = ent->ShouldAutoClearUnavailable();
    ent->url = ui->url->text().trimmed();
    ent->skip_auto_update = ui->skip_auto_update->isChecked();
    ent->sub_options = subOptions;
    ent->front_proxy_id = resolve_proxy_selection(ui->front_proxy, CACHE.front_proxy);
    ent->landing_proxy_id = resolve_proxy_selection(ui->landing_proxy, LANDING.landing_proxy);
    const bool isDefaultGroup = (ent->id == 0 || ent->name.compare(tr("Default"), Qt::CaseInsensitive) == 0 || ent->name.compare("Default", Qt::CaseInsensitive) == 0);
    if (isDefaultGroup && Configs::dataManager != nullptr && Configs::dataManager->settingsRepo != nullptr) {
        bool prevIncludeAll = Configs::dataManager->settingsRepo->default_group_include_all;
        bool newIncludeAll = ui->show_all_nodes->isChecked();
        Configs::dataManager->settingsRepo->default_group_include_all = newIncludeAll;
        Configs::dataManager->settingsRepo->Save();
        if (prevIncludeAll && !newIncludeAll) {
            QList<int> ownProfiles;
            for (int pid : ent->profiles) {
                auto p = Configs::dataManager->profilesRepo->GetProfile(pid);
                if (p != nullptr && p->gid == ent->id) {
                    ownProfiles.append(pid);
                }
            }
            ent->profiles = ownProfiles;
            Configs::dataManager->groupsRepo->Save(ent);
            MW_dialog_message(MwMessage::GroupsChanged, {});
        } else {
            // Also sanitize ent->profiles to remove dead IDs
            QList<int> validProfiles;
            bool purged = false;
            for (int pid : ent->profiles) {
                if (Configs::dataManager->profilesRepo->GetProfile(pid) != nullptr) {
                    validProfiles.append(pid);
                } else {
                    purged = true;
                }
            }
            if (purged) {
                ent->profiles = validProfiles;
                Configs::dataManager->groupsRepo->Save(ent);
                MW_dialog_message(MwMessage::GroupsChanged, {});
            }
        }
    }
    QDialog::accept();
}

QString DialogEditGroup::get_proxy_name(int id) {
    if (id == -1) return "None";
    if (auto profile = Configs::dataManager->profilesRepo->GetProfile(id)) {
        auto group = Configs::dataManager->groupsRepo->GetGroup(profile->gid);
        if (group == nullptr) return "INVALID";
        return QString("[" + group->name + "] ") + profile->name;
    }
    return "INVALID";
}
