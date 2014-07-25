/*
 *  Copyright (c) 2014 Samsung Electronics Co., Ltd All Rights Reserved
 *
 *  Contact: Jan Olszak <j.olszak@samsung.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License
 */

/**
 * @file
 * @author  Jan Olszak (j.olszak@samsung.com)
 * @brief   Definition of the class for managing containers
 */

#include "config.hpp"

#include "host-dbus-definitions.hpp"
#include "common-dbus-definitions.hpp"
#include "container-dbus-definitions.hpp"
#include "containers-manager.hpp"
#include "container-admin.hpp"
#include "exception.hpp"

#include "utils/paths.hpp"
#include "log/logger.hpp"
#include "config/manager.hpp"
#include "dbus/exception.hpp"

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <cassert>
#include <string>
#include <climits>


namespace security_containers {


namespace {

bool regexMatchVector(const std::string& str, const std::vector<boost::regex>& v)
{
    for (const boost::regex& toMatch: v) {
        if (boost::regex_match(str, toMatch)) {
            return true;
        }
    }

    return false;
}

const std::string HOST_ID = "host";

} // namespace

ContainersManager::ContainersManager(const std::string& managerConfigPath): mDetachOnExit(false)
{
    LOGD("Instantiating ContainersManager object...");
    config::loadFromFile(managerConfigPath, mConfig);

    mProxyCallPolicy.reset(new ProxyCallPolicy(mConfig.proxyCallRules));

    using namespace std::placeholders;
    mHostConnection.setProxyCallCallback(bind(&ContainersManager::handleProxyCall,
                                              this, HOST_ID, _1, _2, _3, _4, _5, _6, _7));

    mHostConnection.setGetContainerDbusesCallback(bind(
                &ContainersManager::handleGetContainerDbuses, this, _1));

    mHostConnection.setGetContainerIdsCallback(bind(&ContainersManager::handleGetContainerIdsCall,
                                                    this, _1));

    mHostConnection.setGetActiveContainerIdCallback(bind(&ContainersManager::handleGetActiveContainerIdCall,
                                                         this, _1));

    mHostConnection.setSetActiveContainerCallback(bind(&ContainersManager::handleSetActiveContainerCall,
                                                       this, _1, _2));

    for (auto& containerConfig : mConfig.containerConfigs) {
        std::string containerConfigPath;

        if (containerConfig[0] == '/') {
            containerConfigPath = containerConfig;
        } else {
            std::string baseConfigPath = utils::dirName(managerConfigPath);
            containerConfigPath = utils::createFilePath(baseConfigPath, "/", containerConfig);
        }

        LOGD("Creating Container " << containerConfigPath);
        std::unique_ptr<Container> c(new Container(containerConfigPath,
                                                   mConfig.runMountPointPrefix));
        const std::string id = c->getId();
        if (id == HOST_ID) {
            throw ContainerOperationException("Cannot use reserved container ID");
        }

        c->setNotifyActiveContainerCallback(bind(&ContainersManager::notifyActiveContainerHandler,
                                                 this, id, _1, _2));

        c->setDisplayOffCallback(bind(&ContainersManager::displayOffHandler,
                                      this, id));

        c->setFileMoveRequestCallback(std::bind(&ContainersManager::handleContainerMoveFileRequest,
                                                this, id, _1, _2, _3));

        c->setProxyCallCallback(bind(&ContainersManager::handleProxyCall,
                                     this, id, _1, _2, _3, _4, _5, _6, _7));

        c->setDbusStateChangedCallback(bind(&ContainersManager::handleDbusStateChanged,
                                            this, id, _1));

        mContainers.insert(ContainerMap::value_type(id, std::move(c)));
    }

    // check if default container exists, throw ContainerOperationException if not found
    if (mContainers.find(mConfig.defaultId) == mContainers.end()) {
        LOGE("Provided default container ID " << mConfig.defaultId << " is invalid.");
        throw ContainerOperationException("Provided default container ID " + mConfig.defaultId +
                                          " is invalid.");
    }

    LOGD("ContainersManager object instantiated");

    if (mConfig.inputConfig.enabled) {
        LOGI("Registering input monitor [" << mConfig.inputConfig.device.c_str() << "]");
        mSwitchingSequenceMonitor.reset(
                new InputMonitor(mConfig.inputConfig,
                                 std::bind(&ContainersManager::switchingSequenceMonitorNotify,
                                           this)));
    }
}

ContainersManager::~ContainersManager()
{
    LOGD("Destroying ContainersManager object...");

    if (!mDetachOnExit) {
        try {
            stopAll();
        } catch (ServerException&) {
            LOGE("Failed to stop all of the containers");
        }
    }

    LOGD("ContainersManager object destroyed");
}

void ContainersManager::focus(const std::string& containerId)
{
    /* try to access the object first to throw immediately if it doesn't exist */
    ContainerMap::mapped_type& foregroundContainer = mContainers.at(containerId);

    for (auto& container : mContainers) {
        LOGD(container.second->getId() << ": being sent to background");
        container.second->goBackground();
    }
    mConfig.foregroundId = foregroundContainer->getId();
    LOGD(mConfig.foregroundId << ": being sent to foreground");
    foregroundContainer->goForeground();
}

void ContainersManager::startAll()
{
    LOGI("Starting all containers");

    bool isForegroundFound = false;

    for (auto& container : mContainers) {
        container.second->start();

        if (container.first == mConfig.foregroundId) {
            isForegroundFound = true;
            LOGI(container.second->getId() << ": set as the foreground container");
            container.second->goForeground();
        }
    }

    if (!isForegroundFound) {
        auto foregroundIterator = std::min_element(mContainers.begin(), mContainers.end(),
                                                   [](ContainerMap::value_type &c1, ContainerMap::value_type &c2) {
                                                       return c1.second->getPrivilege() < c2.second->getPrivilege();
                                                   });

        mConfig.foregroundId = foregroundIterator->second->getId();
        LOGI(mConfig.foregroundId << ": no foreground container configured, setting one with highest priority");
        foregroundIterator->second->goForeground();
    }
}

void ContainersManager::stopAll()
{
    LOGI("Stopping all containers");

    for (auto& container : mContainers) {
        container.second->stop();
    }
}

std::string ContainersManager::getRunningForegroundContainerId()
{
    for (auto& container : mContainers) {
        if (container.first == mConfig.foregroundId &&
            container.second->isRunning()) {
            return container.first;
        }
    }
    return std::string();
}

void ContainersManager::switchingSequenceMonitorNotify()
{
    LOGI("switchingSequenceMonitorNotify() called");
    // TODO: implement
}


void ContainersManager::setContainersDetachOnExit()
{
    mDetachOnExit = true;

    for (auto& container : mContainers) {
        container.second->setDetachOnExit();
    }
}

void ContainersManager::notifyActiveContainerHandler(const std::string& caller,
                                                     const std::string& application,
                                                     const std::string& message)
{
    LOGI("notifyActiveContainerHandler(" << caller << ", " << application << ", " << message
         << ") called");
    try {
        const std::string activeContainer = getRunningForegroundContainerId();
        if (!activeContainer.empty() && caller != activeContainer) {
            mContainers[activeContainer]->sendNotification(caller, application, message);
        }
    } catch(const SecurityContainersException&) {
        LOGE("Notification from " << caller << " hasn't been sent");
    }
}

void ContainersManager::displayOffHandler(const std::string& /*caller*/)
{
    // get config of currently set container and switch if switchToDefaultAfterTimeout is true
    const std::string activeContainerName = getRunningForegroundContainerId();
    const auto& activeContainer = mContainers.find(activeContainerName);

    if (activeContainer != mContainers.end() &&
        activeContainer->second->isSwitchToDefaultAfterTimeoutAllowed()) {
        LOGI("Switching to default container " << mConfig.defaultId);
        focus(mConfig.defaultId);
    }
}

void ContainersManager::handleContainerMoveFileRequest(const std::string& srcContainerId,
                                                       const std::string& dstContainerId,
                                                       const std::string& path,
                                                       dbus::MethodResultBuilder::Pointer result)
{
    // TODO: this implementation is only a placeholder.
    // There are too many unanswered questions and security concerns:
    // 1. What about mount namespace, host might not see the source/destination
    //    file. The file might be a different file from a host perspective.
    // 2. Copy vs move (speed and security concerns over already opened FDs)
    // 3. Access to source and destination files - DAC, uid/gig
    // 4. Access to source and destintation files - MAC, smack
    // 5. Destination file uid/gid assignment
    // 6. Destination file smack label assignment
    // 7. Verifiability of the source path

    // NOTE: other possible implementations include:
    // 1. Sending file descriptors opened directly in each container through DBUS
    //    using something like g_dbus_message_set_unix_fd_list()
    // 2. SCS forking and calling setns(MNT) in each container and opening files
    //    by itself, then passing FDs to the main process
    // Now when the main process has obtained FDs (by either of those methods)
    // it can do the copying by itself.

    LOGI("File move requested\n"
         << "src: " << srcContainerId << "\n"
         << "dst: " << dstContainerId << "\n"
         << "path: " << path);

    ContainerMap::const_iterator srcIter = mContainers.find(srcContainerId);
    if (srcIter == mContainers.end()) {
        LOGE("Source container '" << srcContainerId << "' not found");
        return;
    }
    Container& srcContainer = *srcIter->second;

    ContainerMap::const_iterator dstIter = mContainers.find(dstContainerId);
    if (dstIter == mContainers.end()) {
        LOGE("Destination container '" << dstContainerId << "' not found");
        result->set(g_variant_new("(s)", api::container::FILE_MOVE_DESTINATION_NOT_FOUND.c_str()));
        return;
    }
    Container& dstContanier = *dstIter->second;

    if (srcContainerId == dstContainerId) {
        LOGE("Cannot send a file to yourself");
        result->set(g_variant_new("(s)", api::container::FILE_MOVE_WRONG_DESTINATION.c_str()));
        return;
    }

    if (!regexMatchVector(path, srcContainer.getPermittedToSend())) {
        LOGE("Source container has no permissions to send the file: " << path);
        result->set(g_variant_new("(s)", api::container::FILE_MOVE_NO_PERMISSIONS_SEND.c_str()));
        return;
    }

    if (!regexMatchVector(path, dstContanier.getPermittedToRecv())) {
        LOGE("Destination container has no permissions to receive the file: " << path);
        result->set(g_variant_new("(s)", api::container::FILE_MOVE_NO_PERMISSIONS_RECEIVE.c_str()));
        return;
    }

    namespace fs = boost::filesystem;
    std::string srcPath = fs::absolute(srcContainerId, mConfig.containersPath).string() + path;
    std::string dstPath = fs::absolute(dstContainerId, mConfig.containersPath).string() + path;

    if (!utils::moveFile(srcPath, dstPath)) {
        LOGE("Failed to move the file: " << path);
        result->set(g_variant_new("(s)", api::container::FILE_MOVE_FAILED.c_str()));
    } else {
        result->set(g_variant_new("(s)", api::container::FILE_MOVE_SUCCEEDED.c_str()));
        try {
            dstContanier.sendNotification(srcContainerId, path, api::container::FILE_MOVE_SUCCEEDED);
        } catch (ServerException&) {
            LOGE("Notification to '" << dstContainerId << "' has not been sent");
        }
    }
}

void ContainersManager::handleProxyCall(const std::string& caller,
                                        const std::string& target,
                                        const std::string& targetBusName,
                                        const std::string& targetObjectPath,
                                        const std::string& targetInterface,
                                        const std::string& targetMethod,
                                        GVariant* parameters,
                                        dbus::MethodResultBuilder::Pointer result)
{
    if (!mProxyCallPolicy->isProxyCallAllowed(caller,
                                              target,
                                              targetBusName,
                                              targetObjectPath,
                                              targetInterface,
                                              targetMethod)) {
        LOGW("Forbidden proxy call; " << caller << " -> " << target << "; " << targetBusName
                << "; " << targetObjectPath << "; " << targetInterface << "; " << targetMethod);
        result->setError(api::ERROR_FORBIDDEN, "Proxy call forbidden");
        return;
    }

    LOGI("Proxy call; " << caller << " -> " << target << "; " << targetBusName
            << "; " << targetObjectPath << "; " << targetInterface << "; " << targetMethod);

    auto asyncResultCallback = [result](dbus::AsyncMethodCallResult& asyncMethodCallResult) {
        try {
            GVariant* targetResult = asyncMethodCallResult.get();
            result->set(g_variant_new("(v)", targetResult));
        } catch (dbus::DbusException& e) {
            result->setError(api::ERROR_FORWARDED, e.what());
        }
    };

    if (target == HOST_ID) {
        mHostConnection.proxyCallAsync(targetBusName,
                                       targetObjectPath,
                                       targetInterface,
                                       targetMethod,
                                       parameters,
                                       asyncResultCallback);
        return;
    }

    ContainerMap::const_iterator targetIter = mContainers.find(target);
    if (targetIter == mContainers.end()) {
        LOGE("Target container '" << target << "' not found");
        result->setError(api::ERROR_UNKNOWN_ID, "Unknown proxy call target");
        return;
    }

    Container& targetContainer = *targetIter->second;
    targetContainer.proxyCallAsync(targetBusName,
                                   targetObjectPath,
                                   targetInterface,
                                   targetMethod,
                                   parameters,
                                   asyncResultCallback);
}

void ContainersManager::handleGetContainerDbuses(dbus::MethodResultBuilder::Pointer result)
{
    std::vector<GVariant*> entries;
    for (auto& container : mContainers) {
        GVariant* containerId = g_variant_new_string(container.first.c_str());
        GVariant* dbusAddress = g_variant_new_string(container.second->getDbusAddress().c_str());
        GVariant* entry = g_variant_new_dict_entry(containerId, dbusAddress);
        entries.push_back(entry);
    }
    GVariant* dict = g_variant_new_array(G_VARIANT_TYPE("{ss}"), entries.data(), entries.size());
    result->set(g_variant_new("(*)", dict));
}

void ContainersManager::handleDbusStateChanged(const std::string& containerId,
                                               const std::string& dbusAddress)
{
    mHostConnection.signalContainerDbusState(containerId, dbusAddress);
}

void ContainersManager::handleGetContainerIdsCall(dbus::MethodResultBuilder::Pointer result)
{
    std::vector<GVariant*> containerIds;
    for(auto& container: mContainers){
        containerIds.push_back(g_variant_new_string(container.first.c_str()));
    }

    GVariant* array = g_variant_new_array(G_VARIANT_TYPE("s"),
                                          containerIds.data(),
                                          containerIds.size());
    result->set(g_variant_new("(*)", array));
}

void ContainersManager::handleGetActiveContainerIdCall(dbus::MethodResultBuilder::Pointer result)
{
    LOGI("GetActiveContainerId call");
    if (mContainers[mConfig.foregroundId]->isRunning()){
        result->set(g_variant_new("(s)", mConfig.foregroundId.c_str()));
    } else {
        result->set(g_variant_new("(s)", ""));
    }
}

void ContainersManager::handleSetActiveContainerCall(const std::string& id,
                                                     dbus::MethodResultBuilder::Pointer result)
{
    LOGI("SetActiveContainer call; Id=" << id );
    auto container = mContainers.find(id);
    if (container == mContainers.end()){
        LOGE("No container with id=" << id );
        result->setError(api::ERROR_UNKNOWN_ID, "No such container id");
        return;
    }

    if (container->second->isStopped()){
        LOGE("Could not activate a stopped container");
        result->setError(api::host::ERROR_CONTAINER_STOPPED,
                         "Could not activate a stopped container");
        return;
    }

    focus(id);
    result->setVoid();
}

} // namespace security_containers
