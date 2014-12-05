/*
 *  Copyright (c) 2014 Samsung Electronics Co., Ltd All Rights Reserved
 *
 *  Contact: Piotr Bartosiewicz <p.bartosiewi@partner.samsung.com>
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
 * @author  Piotr Bartosiewicz (p.bartosiewi@partner.samsung.com)
 * @brief   Unit tests of the ContainerConnection class
 */

#include "config.hpp"
#include "ut.hpp"

#include "container-connection.hpp"
#include "container-connection-transport.hpp"
#include "host-dbus-definitions.hpp"
#include "container-dbus-definitions.hpp"
// TODO: Switch to real power-manager dbus defs when they will be implemented in power-manager
#include "fake-power-manager-dbus-definitions.hpp"

#include "dbus/connection.hpp"
#include "dbus/exception.hpp"
#include "utils/scoped-daemon.hpp"
#include "utils/glib-loop.hpp"
#include "utils/latch.hpp"
#include "utils/fs.hpp"


BOOST_AUTO_TEST_SUITE(ContainerConnectionSuite)

using namespace vasum;
using namespace vasum::utils;
using namespace dbus;

namespace {

const char* DBUS_DAEMON_PROC = "/usr/bin/dbus-daemon";
const char* const DBUS_DAEMON_ARGS[] = {
    DBUS_DAEMON_PROC,
    "--config-file=" VSM_TEST_CONFIG_INSTALL_DIR "/server/ut-container-connection/ut-dbus.conf",
    "--nofork",
    NULL
};

const std::string TRANSPORT_MOUNT_POINT = "/tmp/ut-container-connection";
const int EVENT_TIMEOUT = 1000;

class ScopedDbusDaemon {
public:
    ScopedDbusDaemon()
        : mTransport(TRANSPORT_MOUNT_POINT)
    {
        mDaemon.start(DBUS_DAEMON_PROC, DBUS_DAEMON_ARGS);
    }

    std::string acquireAddress()
    {
        return mTransport.acquireAddress();
    }
private:
    ContainerConnectionTransport mTransport;
    ScopedDaemon mDaemon;
};

class DbusNameSetter {
public:
    DbusNameSetter()
        : mNameAcquired(false),
          mPendingDisconnect(false)
    {
    }

    void setName(const std::unique_ptr<DbusConnection>& conn, const std::string& name)
    {
        conn->setName(name,
                      std::bind(&DbusNameSetter::onNameAcquired, this),
                      std::bind(&DbusNameSetter::onDisconnect, this));

        if(!waitForName()) {
            throw dbus::DbusOperationException("Could not acquire name.");
        }
    }

    bool waitForName()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mNameCondition.wait(lock, [this] {return mNameAcquired || mPendingDisconnect;});
        return mNameAcquired;
    }

    void onNameAcquired()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mNameAcquired = true;
        mNameCondition.notify_one();
    }

    void onDisconnect()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mPendingDisconnect = true;
        mNameCondition.notify_one();
    }

private:
    bool mNameAcquired;
    bool mPendingDisconnect;
    std::mutex mMutex;
    std::condition_variable mNameCondition;
};

} // namespace


BOOST_AUTO_TEST_CASE(ConstructorDestructorConnectTest)
{
    ScopedGlibLoop loop;
    ScopedDbusDaemon dbus;

    BOOST_REQUIRE_NO_THROW(ContainerConnection(dbus.acquireAddress(), nullptr));
}

BOOST_AUTO_TEST_CASE(NotifyActiveContainerApiTest)
{
    ScopedGlibLoop loop;
    ScopedDbusDaemon dbus;

    Latch notifyCalled;
    std::unique_ptr<ContainerConnection> connection;

    BOOST_REQUIRE_NO_THROW(connection.reset(new ContainerConnection(dbus.acquireAddress(), nullptr)));

    auto callback = [&](const std::string& application, const std::string& message) {
        if (application == "testapp" && message == "testmessage") {
            notifyCalled.set();
        }
    };
    connection->setNotifyActiveContainerCallback(callback);

    DbusConnection::Pointer client = DbusConnection::create(dbus.acquireAddress());
    client->callMethod(api::container::BUS_NAME,
                       api::container::OBJECT_PATH,
                       api::container::INTERFACE,
                       api::container::METHOD_NOTIFY_ACTIVE_CONTAINER,
                       g_variant_new("(ss)", "testapp", "testmessage"),
                       "()");
    BOOST_CHECK(notifyCalled.wait(EVENT_TIMEOUT));
}

BOOST_AUTO_TEST_CASE(SignalNotificationApiTest)
{
    ScopedGlibLoop loop;
    ScopedDbusDaemon dbus;

    Latch signalEmitted;
    std::unique_ptr<ContainerConnection> connection;

    BOOST_REQUIRE_NO_THROW(connection.reset(new ContainerConnection(dbus.acquireAddress(), nullptr)));

    DbusConnection::Pointer client = DbusConnection::create(dbus.acquireAddress());

    auto handler = [&](const std::string& /*senderBusName*/,
                       const std::string& objectPath,
                       const std::string& interface,
                       const std::string& signalName,
                       GVariant* parameters) {
        if (objectPath == api::container::OBJECT_PATH &&
            interface == api::container::INTERFACE &&
            signalName == api::container::SIGNAL_NOTIFICATION &&
            g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) {

            const gchar* container = NULL;
            const gchar* application = NULL;
            const gchar* message = NULL;
            g_variant_get(parameters, "(&s&s&s)", &container, &application, &message);
            if (container == std::string("testcontainer") &&
                application == std::string("testapp") &&
                message == std::string("testmessage")) {
                signalEmitted.set();
            }
        }
    };
    client->signalSubscribe(handler, api::container::BUS_NAME);

    connection->sendNotification("testcontainer", "testapp", "testmessage");

    BOOST_CHECK(signalEmitted.wait(EVENT_TIMEOUT));
}

BOOST_AUTO_TEST_CASE(SignalDisplayOffApiTest)
{
    ScopedGlibLoop loop;
    ScopedDbusDaemon dbus;

    Latch displayOffCalled;
    std::unique_ptr<ContainerConnection> connection;

    BOOST_REQUIRE_NO_THROW(connection.reset(new ContainerConnection(dbus.acquireAddress(),
                                            nullptr)));

    DbusConnection::Pointer client = DbusConnection::create(dbus.acquireAddress());

    auto callback = [&]() {
        displayOffCalled.set();
    };

    connection->setDisplayOffCallback(callback);

    client->emitSignal(fake_power_manager_api::OBJECT_PATH,
                       fake_power_manager_api::INTERFACE,
                       fake_power_manager_api::SIGNAL_DISPLAY_OFF,
                       nullptr);

    // timeout should occur, since no name is set to client
    BOOST_CHECK(!displayOffCalled.wait(EVENT_TIMEOUT));

    DbusNameSetter setter;

    setter.setName(client, fake_power_manager_api::BUS_NAME);

    client->emitSignal(fake_power_manager_api::OBJECT_PATH,
                       fake_power_manager_api::INTERFACE,
                       fake_power_manager_api::SIGNAL_DISPLAY_OFF,
                       nullptr);

    // now signal should be delivered correctly
    BOOST_CHECK(displayOffCalled.wait(EVENT_TIMEOUT));
}


BOOST_AUTO_TEST_SUITE_END()
