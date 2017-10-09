/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "statsd"
#define DEBUG true

#include "StatsService.h"
#include "DropboxReader.h"

#include <android-base/file.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <cutils/log.h>
#include <frameworks/base/cmds/statsd/src/statsd_config.pb.h>
#include <private/android_filesystem_config.h>
#include <utils/Looper.h>
#include <utils/String16.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace android;

namespace android {
namespace os {
namespace statsd {

StatsService::StatsService(const sp<Looper>& handlerLooper)
    : mAnomalyMonitor(new AnomalyMonitor(2)), mStatsPullerManager()  // TODO: Change this based on the config
{
    ALOGD("stats service constructed");
}

StatsService::~StatsService() {
}

status_t StatsService::setProcessor(const sp<StatsLogProcessor>& main_processor) {
    m_processor = main_processor;
    ALOGD("stats service set to processor %p", m_processor.get());
    return NO_ERROR;
}

// Implement our own because the default binder implementation isn't
// properly handling SHELL_COMMAND_TRANSACTION
status_t StatsService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                  uint32_t flags) {
    status_t err;

    switch (code) {
        case SHELL_COMMAND_TRANSACTION: {
            int in = data.readFileDescriptor();
            int out = data.readFileDescriptor();
            int err = data.readFileDescriptor();
            int argc = data.readInt32();
            Vector<String8> args;
            for (int i = 0; i < argc && data.dataAvail() > 0; i++) {
                args.add(String8(data.readString16()));
            }
            sp<IShellCallback> shellCallback = IShellCallback::asInterface(data.readStrongBinder());
            sp<IResultReceiver> resultReceiver =
                    IResultReceiver::asInterface(data.readStrongBinder());

            FILE* fin = fdopen(in, "r");
            FILE* fout = fdopen(out, "w");
            FILE* ferr = fdopen(err, "w");

            if (fin == NULL || fout == NULL || ferr == NULL) {
                resultReceiver->send(NO_MEMORY);
            } else {
                err = command(fin, fout, ferr, args);
                resultReceiver->send(err);
            }

            if (fin != NULL) {
                fflush(fin);
                fclose(fin);
            }
            if (fout != NULL) {
                fflush(fout);
                fclose(fout);
            }
            if (fout != NULL) {
                fflush(ferr);
                fclose(ferr);
            }

            return NO_ERROR;
        }
        default: { return BnStatsManager::onTransact(code, data, reply, flags); }
    }
}

status_t StatsService::dump(int fd, const Vector<String16>& args) {
    FILE* out = fdopen(fd, "w");
    if (out == NULL) {
        return NO_MEMORY;  // the fd is already open
    }

    fprintf(out, "StatsService::dump:");
    ALOGD("StatsService::dump:");
    const int N = args.size();
    for (int i = 0; i < N; i++) {
        fprintf(out, " %s", String8(args[i]).string());
        ALOGD("   %s", String8(args[i]).string());
    }
    fprintf(out, "\n");

    fclose(out);
    return NO_ERROR;
}

status_t StatsService::command(FILE* in, FILE* out, FILE* err, Vector<String8>& args) {
    if (args.size() > 0) {
        if (!args[0].compare(String8("print-stats-log")) && args.size() > 1) {
            return doPrintStatsLog(out, args);
        }
        if (!args[0].compare(String8("config"))) {
            return doLoadConfig(in);
        }
    }

    printCmdHelp(out);
    return NO_ERROR;
}

status_t StatsService::doLoadConfig(FILE* in) {
    string content;
    if (!android::base::ReadFdToString(fileno(in), &content)) {
        return UNKNOWN_ERROR;
    }
    StatsdConfig config;
    if (config.ParseFromString(content)) {
        ALOGD("Config parsed from command line: %s", config.SerializeAsString().c_str());
        m_processor->UpdateConfig(0, config);
        return NO_ERROR;
    } else {
        ALOGD("Config failed to be parsed");
        return UNKNOWN_ERROR;
    }
}

Status StatsService::informAnomalyAlarmFired() {
    if (DEBUG) ALOGD("StatsService::informAnomalyAlarmFired was called");

    if (IPCThreadState::self()->getCallingUid() != AID_SYSTEM) {
        return Status::fromExceptionCode(Status::EX_SECURITY,
                                         "Only system uid can call informAnomalyAlarmFired");
    }

    if (DEBUG) ALOGD("StatsService::informAnomalyAlarmFired succeeded");
    // TODO: check through all counters/timers and see if an anomaly has indeed occurred.

    return Status::ok();
}

Status StatsService::informPollAlarmFired() {
    if (DEBUG) ALOGD("StatsService::informPollAlarmFired was called");

    if (IPCThreadState::self()->getCallingUid() != AID_SYSTEM) {
        return Status::fromExceptionCode(Status::EX_SECURITY,
                                         "Only system uid can call informPollAlarmFired");
    }

    if (DEBUG) ALOGD("StatsService::informPollAlarmFired succeeded");
    // TODO: determine what services to poll and poll (or ask StatsCompanionService to poll) them.
    String16 output = mStatsPullerManager.pull(StatsPullerManager::KERNEL_WAKELOCKS);
    // TODO: do something useful with the output instead of writing a string to screen.
    ALOGD("%s", String8(output).string());
    ALOGD("%d", int(output.size()));

    return Status::ok();
}

Status StatsService::systemRunning() {
    if (IPCThreadState::self()->getCallingUid() != AID_SYSTEM) {
        return Status::fromExceptionCode(Status::EX_SECURITY,
                                         "Only system uid can call systemRunning");
    }

    // When system_server is up and running, schedule the dropbox task to run.
    ALOGD("StatsService::systemRunning");

    sayHiToStatsCompanion();

    return Status::ok();
}

void StatsService::sayHiToStatsCompanion() {
    // TODO: This method needs to be private. It is temporarily public and unsecured for testing
    // purposes.
    sp<IStatsCompanionService> statsCompanion = getStatsCompanionService();
    if (statsCompanion != nullptr) {
        if (DEBUG) ALOGD("Telling statsCompanion that statsd is ready");
        statsCompanion->statsdReady();
    } else {
        if (DEBUG) ALOGD("Could not access statsCompanion");
    }
}

sp<IStatsCompanionService> StatsService::getStatsCompanionService() {
    sp<IStatsCompanionService> statsCompanion = nullptr;
    // Get statscompanion service from service manager
    const sp<IServiceManager> sm(defaultServiceManager());
    if (sm != nullptr) {
        const String16 name("statscompanion");
        statsCompanion = interface_cast<IStatsCompanionService>(sm->checkService(name));
        if (statsCompanion == nullptr) {
            ALOGW("statscompanion service unavailable!");
            return nullptr;
        }
    }
    return statsCompanion;
}

Status StatsService::statsCompanionReady() {
    if (DEBUG) ALOGD("StatsService::statsCompanionReady was called");

    if (IPCThreadState::self()->getCallingUid() != AID_SYSTEM) {
        return Status::fromExceptionCode(Status::EX_SECURITY,
                                         "Only system uid can call statsCompanionReady");
    }

    sp<IStatsCompanionService> statsCompanion = getStatsCompanionService();
    if (statsCompanion == nullptr) {
        return Status::fromExceptionCode(
                Status::EX_NULL_POINTER,
                "statscompanion unavailable despite it contacting statsd!");
    }
    if (DEBUG) ALOGD("StatsService::statsCompanionReady linking to statsCompanion.");
    IInterface::asBinder(statsCompanion)->linkToDeath(new StatsdDeathRecipient(mAnomalyMonitor));
    mAnomalyMonitor->setStatsCompanionService(statsCompanion);

    return Status::ok();
}

void StatsdDeathRecipient::binderDied(const wp<IBinder>& who) {
    ALOGW("statscompanion service died");
    mAnmlyMntr->setStatsCompanionService(nullptr);
}

status_t StatsService::doPrintStatsLog(FILE* out, const Vector<String8>& args) {
    long msec = 0;

    if (args.size() > 2) {
        msec = strtol(args[2].string(), NULL, 10);
    }
    return DropboxReader::readStatsLogs(out, args[1].string(), msec);
}

void StatsService::printCmdHelp(FILE* out) {
    fprintf(out, "Usage:\n");
    fprintf(out, "\t print-stats-log [tag_required] [timestamp_nsec_optional]\n");
    fprintf(out,
            "\t config\t Loads a new config from command-line (must be proto in wire-encoded "
            "format).\n");
}

}  // namespace statsd
}  // namespace os
}  // namespace android
