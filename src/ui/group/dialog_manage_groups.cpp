#include "include/ui/group/dialog_manage_groups.h"

#include "include/configs/sub/GroupUpdater.hpp"
#include "include/global/GuiUtils.hpp"
#include "include/ui/group/GroupItem.h"
#include "include/ui/group/dialog_edit_group.h"

#include <QInputDialog>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QScreen>
#include <QFontMetrics>

#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/SettingsRepo.h"


#define AddGroupToListIfExist(_id)                       \
    auto __ent = Configs::dataManager->groupsRepo->GetGroup(_id); \
    if (__ent != nullptr) {                              \
        auto wI = new QListWidgetItem();                 \
        auto w = new GroupItem(this, __ent, wI);         \
        wI->setData(114514, _id);                        \
        ui->listWidget->addItem(wI);                     \
        ui->listWidget->setItemWidget(wI, w);            \
    }

DialogManageGroups::DialogManageGroups(QWidget *parent) : QDialog(parent), ui(new Ui::DialogManageGroups) {
    ui->setupUi(this);

    if (Configs::dataManager != nullptr && Configs::dataManager->settingsRepo != nullptr &&
        !Configs::dataManager->settingsRepo->groupsDialogGeometry.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(Configs::dataManager->settingsRepo->groupsDialogGeometry.toUtf8()));
    } else {
        const QFontMetrics fm(font());
        const int targetWidth = qMax(680, fm.horizontalAdvance('W') * 46);
        const int targetHeight = qMax(500, fm.height() * 26);
        const QSize avail = screen() ? screen()->availableGeometry().size() : QSize(1920, 1080);
        resize(QSize(targetWidth, targetHeight).boundedTo(avail));
    }

    for (auto id: Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        AddGroupToListIfExist(id)
    }

    connect(ui->listWidget, &QListWidget::itemDoubleClicked, this, [=,this](QListWidgetItem *wI) {
        auto w = dynamic_cast<GroupItem *>(ui->listWidget->itemWidget(wI));
        emit w->edit_clicked();
    });
}

DialogManageGroups::~DialogManageGroups() {
    if (Configs::dataManager != nullptr && Configs::dataManager->settingsRepo != nullptr) {
        Configs::dataManager->settingsRepo->groupsDialogGeometry = QString::fromUtf8(saveGeometry().toBase64());
        Configs::dataManager->settingsRepo->Save();
    }
    delete ui;
}

void DialogManageGroups::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    for (int i = 0; i < ui->listWidget->count(); ++i) {
        auto* item = ui->listWidget->item(i);
        if (auto* w = ui->listWidget->itemWidget(item)) {
            w->adjustSize();
            item->setSizeHint(w->sizeHint());
        }
    }
    ui->listWidget->doItemsLayout();
}

void DialogManageGroups::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        for (int i = 0; i < ui->listWidget->count(); ++i) {
            auto* item = ui->listWidget->item(i);
            if (auto* w = ui->listWidget->itemWidget(item)) {
                w->adjustSize();
                item->setSizeHint(w->sizeHint());
            }
        }
        ui->listWidget->doItemsLayout();
    }
}

void DialogManageGroups::on_add_clicked() {
    auto ent = Configs::dataManager->groupsRepo->NewGroup();
    auto dialog = new DialogEditGroup(ent, this);
    int ret = dialog->exec();
    dialog->deleteLater();

    if (ret == QDialog::Accepted) {
        Configs::dataManager->groupsRepo->AddGroup(ent);
        AddGroupToListIfExist(ent->id);
        MW_dialog_message(MwMessage::GroupsChanged, {});
    }
}

void DialogManageGroups::on_update_all_clicked() {
    if (QMessageBox::question(this, tr("Confirmation"), tr("Update all subscriptions?")) == QMessageBox::StandardButton::Yes) {
        Subscription::updater()->RefreshAll();
    }
}
