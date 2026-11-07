#include "include/ui/mainwindow.h"

#include "include/configs/generate.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/engine/connection/ConnectionManager.h"
#include "include/engine/tests/TestRunner.h"

int MainWindow::get_profile_to_start() {
    const auto ents = get_now_selected_list();
    if (ents.size() == 1) {
        return ents.first();
    }
    if (ents.isEmpty()) {
        if (connection()->lastProfileId() >= 0 && Configs::dataManager->profilesRepo->GetProfile(connection()->lastProfileId()) != nullptr) {
            return connection()->lastProfileId();
        }
        const int rememberId = Configs::dataManager->settingsRepo->remember_id;
        if (rememberId >= 0 && Configs::dataManager->profilesRepo->GetProfile(rememberId) != nullptr) {
            return rememberId;
        }
        const auto currentGroup = Configs::dataManager->groupsRepo->CurrentGroup();
        if (currentGroup) {
            const auto profiles = currentGroup->Profiles();
            if (!profiles.isEmpty()) {
                const int firstId = profiles.first();
                if (Configs::dataManager->profilesRepo->GetProfile(firstId) != nullptr) {
                    return firstId;
                }
            }
        }
    }
    return -1;
}

void MainWindow::profile_start(int _id) {
    if (_id < 0) _id = get_profile_to_start();
    if (select_mode && !Configs::dataManager->settingsRepo->prepare_exit && _id >= 0 &&
        Configs::dataManager->profilesRepo->GetProfile(_id) != nullptr) {
        finish_select_mode(_id);
        return;
    }
    connection()->start(StartRequest{_id});
}

void MainWindow::profile_stop(bool crash, bool block, bool manual, bool interactive) {
    connection()->stop({crash, block, manual, interactive});
}

std::shared_ptr<Configs::Profile> MainWindow::runningSnapshot() const {
    return connection()->runningSnapshot();
}

void MainWindow::toggle_connection() {
    const auto state = connection()->connectionState();
    if (select_mode || state == ConnectionState::Connecting || state == ConnectionState::Stopping) return;
    if (runningSnapshot() != nullptr) {
        profile_stop(false, false, true);
        return;
    }
    profile_start(connection()->resolveLastProfile());
}

void MainWindow::evaluateSmartFailover(const std::shared_ptr<Configs::Profile>& ent) {
    if (!Configs::dataManager->settingsRepo->smart_failover_enabled) return;
    auto currRunning = runningSnapshot();
    if (currRunning == nullptr || ent == nullptr || ent->id != currRunning->id) return;
    if (connection()->connectionState() == ConnectionState::Connecting) return;

    // Check anti-flapping cooldown (15 seconds) using atomic compare exchange
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 lastTime = m_lastFailoverTime.load();
    if (now - lastTime < 15) return;

    bool shouldFailover = false;
    QString triggerReason;

    const int spikeThreshold = Configs::dataManager->settingsRepo->smart_failover_latency_spike_ms;

    // Condition 1: Continuous failures or complete failure
    if (ent->failed_count >= 2 || ent->latency < 0) {
        shouldFailover = true;
        if (ent->failed_count >= 2) {
            triggerReason = tr("continuous failures (%1 times)").arg(ent->failed_count);
        } else {
            triggerReason = tr("connection failure");
        }
    }
    // Condition 2: Latency spike
    else if (ent->latency > spikeThreshold) {
        shouldFailover = true;
        triggerReason = tr("high latency spike (%1 ms > %2 ms)").arg(ent->latency).arg(spikeThreshold);
    }

    if (!shouldFailover) return;

    // Atomic guard to prevent multiple threads from competing
    if (!m_lastFailoverTime.compare_exchange_strong(lastTime, now)) return;

    // Search for healthiest candidate in the SAME group
    const int currentGid = ent->gid;
    auto group = Configs::dataManager->groupsRepo->GetGroup(currentGid);
    if (!group) return;

    QList<int> memberIds = group->Profiles();
    if (memberIds.size() <= 1) {
        MW_show_log(tr("[Smart Failover] No alternative nodes available in group '%1'.").arg(group->name));
        return;
    }

    auto candidates = Configs::dataManager->profilesRepo->GetProfileBatch(memberIds);
    std::shared_ptr<Configs::Profile> bestCandidate = nullptr;
    int bestLatency = 999999;

    for (const auto& cand : candidates) {
        if (!cand || cand->id == ent->id) continue;
        if (!Configs::IsValid(cand)) continue;

        // Skip candidates that are already failing or unavailable
        if (cand->failed_count > 0 || cand->latency < 0) continue;

        // Prefer candidates with measured positive latency
        if (cand->latency > 0 && cand->latency != Configs::kLatencyConnectOnly) {
            if (cand->latency < bestLatency) {
                bestLatency = cand->latency;
                bestCandidate = cand;
            }
        }
    }

    // Fallback: If no node has positive latency, check for connect-only or untested healthy nodes (failed_count == 0)
    if (!bestCandidate) {
        for (const auto& cand : candidates) {
            if (!cand || cand->id == ent->id) continue;
            if (!Configs::IsValid(cand)) continue;
            if (cand->failed_count == 0 && cand->latency >= 0) {
                bestCandidate = cand;
                break;
            }
        }
    }

    if (!bestCandidate) {
        MW_show_log(tr("[Smart Failover] No healthy alternate node found in group '%1' to switch to.").arg(group->name));
        return;
    }

    const QString oldName = ent->outbound ? ent->outbound->DisplayTypeAndName() : ent->name;
    const QString newName = bestCandidate->outbound ? bestCandidate->outbound->DisplayTypeAndName() : bestCandidate->name;
    const int newId = bestCandidate->id;
    const int candidateLatency = bestCandidate->latency;

    MW_show_log(tr("[Smart Failover] Triggered: Node '%1' %2. Automatically switching to '%3' (%4 ms)...")
                    .arg(oldName, triggerReason, newName, candidateLatency > 0 ? QString::number(candidateLatency) : tr("OK")));

    // Dispatch switch on UI thread
    runOnUiThread([=, this] {
        const auto live = runningSnapshot();
        if (live == nullptr || live->id != ent->id) return;

        // Send tray notification
        if (tray) {
            QString msg = tr("Active node '%1' experienced %2.\nSwitched to: '%3' (%4 ms)")
                              .arg(oldName, triggerReason, newName, candidateLatency > 0 ? QString::number(candidateLatency) : tr("OK"));
            tray->showMessage(tr("Smart Failover"), msg, QSystemTrayIcon::Information, 8000);
        }

        // Seamlessly start the target candidate
        profile_start(newId);
    });
}

void MainWindow::checkActiveNodeHealth() {
    if (!Configs::dataManager->settingsRepo->smart_failover_enabled) return;
    const auto running = runningSnapshot();
    if (running == nullptr || !Configs::dataManager->settingsRepo->core_running) return;
    if (connection()->connectionState() == ConnectionState::Connecting || tests()->isRunning()) return;

    const int spikeThreshold = Configs::dataManager->settingsRepo->smart_failover_latency_spike_ms;
    if (running->failed_count >= 2 || running->latency < 0 || running->latency > spikeThreshold) {
        evaluateSmartFailover(running);
        return;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (running->latency_at == 0 || (now - running->latency_at > 60)) {
        tests()->runUrlTests({running->id}, {}, false);
    }
}
