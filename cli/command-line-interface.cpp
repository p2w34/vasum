/*
 *  Copyright (c) 2014 Samsung Electronics Co., Ltd All Rights Reserved
 *
 *  Contact: Mateusz Malicki <m.malicki2@samsung.com>
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
 * @author  Mateusz Malicki (m.malicki2@samsung.com)
 * @brief   Definition of CommandLineInterface class
 */

#include "config.hpp"
#include "command-line-interface.hpp"
#include "vasum-client.h"

#include <map>
#include <stdexcept>
#include <functional>
#include <ostream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <fcntl.h>
#include <cassert>
#include <linux/if_link.h>
#include <arpa/inet.h>

using namespace std;

namespace vasum {
namespace cli {

VsmClient CommandLineInterface::client = NULL;

namespace {

template<typename T>
string stringAsInStream(const T& value)
{
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

ostream& operator<<(ostream& out, const VsmZoneState& state)
{
    const char* name;
    switch (state) {
        case STOPPED: name = "STOPPED"; break;
        case STARTING: name = "STARTING"; break;
        case RUNNING: name = "RUNNING"; break;
        case STOPPING: name = "STOPPING"; break;
        case ABORTING: name = "ABORTING"; break;
        case FREEZING: name = "FREEZING"; break;
        case FROZEN: name = "FROZEN"; break;
        case THAWED: name = "THAWED"; break;
        case LOCKED: name = "LOCKED"; break;
        case MAX_STATE: name = "MAX_STATE"; break;
        case ACTIVATING: name = "ACTIVATING"; break;
        default: name = "MAX_STATE (ERROR)";
    }

    out << name;
    return out;
}

ostream& operator<<(ostream& out, const VsmZone& zone)
{
    out << "Name: " << zone->id
        << "\nTerminal: " << zone->terminal
        << "\nState: " << zone->state
        << "\nRoot: " << zone->rootfs_path;
    return out;
}

ostream& operator<<(ostream& out, const VsmNetdevType& netdevType)
{
    switch (netdevType) {
        case VSMNETDEV_VETH: out << "VETH"; break;
        case VSMNETDEV_PHYS: out << "PHYS"; break;
        case VSMNETDEV_MACVLAN: out << "MACVLAN"; break;
    }
    return out;
}

ostream& operator<<(ostream& out, const VsmNetdev& netdev)
{
    out << "Name: " << netdev->name
        << "\nType: " << netdev->type;
    return out;
}

typedef vector<vector<string>> Table;

ostream& operator<<(ostream& out, const Table& table)
{
    vector<size_t> sizes;
    for (const auto& row : table) {
        if (sizes.size() < row.size()) {
            sizes.resize(row.size());
        }
        for (size_t i = 0; i < row.size(); ++i) {
            sizes[i] = max(sizes[i], row[i].length());
        }
    }

    for (const auto& row : table) {
        for (size_t i = 0; i < row.size(); ++i) {
            out << left << setw(sizes[i]+2) << row[i];
        }
        out << "\n";
    }

    return out;
}

enum macvlan_mode macvlanFromString(const std::string& mode) {
    if (mode == "private") {
        return MACVLAN_MODE_PRIVATE;
    }
    if (mode == "vepa") {
        return MACVLAN_MODE_VEPA;
    }
    if (mode == "bridge") {
        return MACVLAN_MODE_BRIDGE;
    }
    if (mode == "passthru") {
        return MACVLAN_MODE_PASSTHRU;
    }
    throw runtime_error("Unsupported macvlan mode");
}

} // namespace

void CommandLineInterface::connect()
{
    VsmStatus status;

    CommandLineInterface::client = vsm_client_create();
    if (NULL == CommandLineInterface::client) {
        throw runtime_error("Can't create client");
    }

    status = vsm_connect(client);
    if (VSMCLIENT_SUCCESS != status) {
        string msg = vsm_get_status_message(CommandLineInterface::client);
        vsm_client_free(CommandLineInterface::client);
        CommandLineInterface::client = NULL;
        throw runtime_error(msg);
    }
}

void CommandLineInterface::disconnect()
{
    string msg;
    VsmStatus status;

    status = vsm_disconnect(CommandLineInterface::client);
    if (VSMCLIENT_SUCCESS != status) {
        msg = vsm_get_status_message(CommandLineInterface::client);
    }

    vsm_client_free(CommandLineInterface::client);
    CommandLineInterface::client = NULL;

    if (VSMCLIENT_SUCCESS != status) {
        throw runtime_error(msg);
    }
}

void CommandLineInterface::executeCallback(const function<VsmStatus(VsmClient)>& fun)
{
    VsmStatus status;

    status = fun(CommandLineInterface::client);
    if (VSMCLIENT_SUCCESS != status) {
        throw runtime_error(vsm_get_status_message(CommandLineInterface::client));
    }
}

const std::string& CommandLineInterface::getName() const
{
    return mName;
}

const std::string& CommandLineInterface::getDescription() const
{
    return mDescription;
}

void CommandLineInterface::printUsage(std::ostream& out) const
{
    out << mName;
    for (const auto& args : mArgsSpec) {
        out << " " << args.first;
    }

    out << "\n\n"
        << "\tDescription\n"
        << "\t\t" << mDescription << "\n";

    if (!mArgsSpec.empty()) {
        out << "\n\tOptions\n";
        for (const auto& args : mArgsSpec) {
            out << "\t\t" << args.first << " -- " << args.second << "\n";
        }
    }
    out << "\n";
}

bool CommandLineInterface::isAvailable(unsigned int mode) const
{
    return (mAvailability & mode) == mode;
}

void CommandLineInterface::execute(const Args& argv)
{
    mExecutorCallback(argv);
}


void lock_queue(const Args& /*argv*/)
{
    CommandLineInterface::executeCallback(vsm_lock_queue);
}

void unlock_queue(const Args& /*argv*/)
{
    CommandLineInterface::executeCallback(vsm_unlock_queue);
}

void set_active_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_set_active_zone, _1, argv[1].c_str()));
}

void create_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    if (argv.size() >= 3 && !argv[2].empty()) {
        CommandLineInterface::executeCallback(bind(vsm_create_zone, _1, argv[1].c_str(), argv[2].c_str()));
    } else {
        CommandLineInterface::executeCallback(bind(vsm_create_zone, _1, argv[1].c_str(), nullptr));
    }
}

void destroy_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_destroy_zone, _1, argv[1].c_str(), 1));
}

void shutdown_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_shutdown_zone, _1, argv[1].c_str()));
}

void start_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_start_zone, _1, argv[1].c_str()));
}

void lock_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_lock_zone, _1, argv[1].c_str()));
}

void unlock_zone(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_unlock_zone, _1, argv[1].c_str()));
}

void get_zones_status(const Args& /* argv */)
{
    using namespace std::placeholders;

    VsmArrayString ids;
    VsmString activeId;
    Table table;

    CommandLineInterface::executeCallback(bind(vsm_get_zone_ids, _1, &ids));
    CommandLineInterface::executeCallback(bind(vsm_get_active_zone_id, _1, &activeId));
    table.push_back({"Active", "Id", "State", "Terminal", "Root"});
    for (VsmString* id = ids; *id; ++id) {
        VsmZone zone;
        CommandLineInterface::executeCallback(bind(vsm_lookup_zone_by_id, _1, *id, &zone));
        assert(string(zone->id) == string(*id));
        table.push_back({string(zone->id) == string(activeId) ? "*" : "",
                         zone->id,
                         stringAsInStream(zone->state),
                         to_string(zone->terminal),
                         zone->rootfs_path});
        vsm_zone_free(zone);
    }
    vsm_string_free(activeId);
    vsm_array_string_free(ids);
    cout << table << endl;
}

void get_zone_ids(const Args& /*argv*/)
{
    using namespace std::placeholders;

    VsmArrayString ids;
    CommandLineInterface::executeCallback(bind(vsm_get_zone_ids, _1, &ids));
    string delim;
    for (VsmString* id = ids; *id; ++id) {
        cout << delim << *id;
        delim = ", ";
    }
    cout << endl;
    vsm_array_string_free(ids);
}

void get_active_zone_id(const Args& /*argv*/)
{
    using namespace std::placeholders;

    VsmString id;
    CommandLineInterface::executeCallback(bind(vsm_get_active_zone_id, _1, &id));
    cout << id << endl;
    vsm_string_free(id);
}

void lookup_zone_by_id(const Args& argv)
{
    using namespace std::placeholders;
    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }

    VsmZone zone;
    CommandLineInterface::executeCallback(bind(vsm_lookup_zone_by_id, _1, argv[1].c_str(), &zone));
    cout << zone << endl;
    vsm_zone_free(zone);
}

void grant_device(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }

    uint32_t flags = O_RDWR;
    CommandLineInterface::executeCallback(bind(vsm_grant_device, _1, argv[1].c_str(), argv[2].c_str(), flags));
}

void revoke_device(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }

    CommandLineInterface::executeCallback(bind(vsm_revoke_device, _1, argv[1].c_str(), argv[2].c_str()));
}

void create_netdev_veth(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 3) {
        throw runtime_error("Not enough parameters");
    }
    CommandLineInterface::executeCallback(bind(vsm_create_netdev_veth,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  argv[3].c_str()));
}

void create_netdev_macvlan(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 4) {
        throw runtime_error("Not enough parameters");
    }
    CommandLineInterface::executeCallback(bind(vsm_create_netdev_macvlan,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  argv[3].c_str(),
                  macvlanFromString(argv[4].c_str())));
}

void create_netdev_phys(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    CommandLineInterface::executeCallback(bind(vsm_create_netdev_phys,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str()));
}

void lookup_netdev_by_name(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    VsmNetdev vsmNetdev = NULL;
    CommandLineInterface::executeCallback(bind(vsm_lookup_netdev_by_name,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  &vsmNetdev));
    cout << vsmNetdev << endl;
    vsm_netdev_free(vsmNetdev);

}

void destroy_netdev(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    CommandLineInterface::executeCallback(bind(vsm_destroy_netdev,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str()));
}

void zone_get_netdevs(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 1) {
        throw runtime_error("Not enough parameters");
    }
    VsmArrayString ids;
    CommandLineInterface::executeCallback(bind(vsm_zone_get_netdevs,
                  _1,
                  argv[1].c_str(),
                  &ids));
    string delim;
    for (VsmString* id = ids; *id; ++id) {
        cout << delim << *id;
        delim = ", ";
    }
    if (delim.empty()) {
        cout << "There is no network device in zone";
    }
    cout << endl;
    vsm_array_string_free(ids);
}

void netdev_get_ipv4_addr(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    in_addr addr;
    CommandLineInterface::executeCallback(bind(vsm_netdev_get_ipv4_addr,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  &addr));
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr, buf, INET_ADDRSTRLEN) == NULL) {
        throw runtime_error("Server gave the wrong address format");
    }
    cout << buf << endl;
}

void netdev_get_ipv6_addr(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    in6_addr addr;
    CommandLineInterface::executeCallback(bind(vsm_netdev_get_ipv6_addr,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  &addr));
    char buf[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, &addr, buf, INET6_ADDRSTRLEN) == NULL) {
        throw runtime_error("Server gave the wrong address format");
    }
    cout << buf << endl;
}

void netdev_set_ipv4_addr(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 4) {
        throw runtime_error("Not enough parameters");
    }
    in_addr addr;
    if (inet_pton(AF_INET, argv[3].c_str(), &addr) != 1) {
        throw runtime_error("Wrong address format");
    };
    CommandLineInterface::executeCallback(bind(vsm_netdev_set_ipv4_addr,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  &addr,
                  stoi(argv[4].c_str())));
}

void netdev_set_ipv6_addr(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 4) {
        throw runtime_error("Not enough parameters");
    }
    in6_addr addr;
    if (inet_pton(AF_INET6, argv[3].c_str(), &addr) != 1) {
        throw runtime_error("Wrong address format");
    };
    CommandLineInterface::executeCallback(bind(vsm_netdev_set_ipv6_addr,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str(),
                  &addr,
                  stoi(argv[4].c_str())));
}

void netdev_up(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    CommandLineInterface::executeCallback(bind(vsm_netdev_up,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str()));
}

void netdev_down(const Args& argv)
{
    using namespace std::placeholders;

    if (argv.size() <= 2) {
        throw runtime_error("Not enough parameters");
    }
    CommandLineInterface::executeCallback(bind(vsm_netdev_down,
                  _1,
                  argv[1].c_str(),
                  argv[2].c_str()));
}

} // namespace cli
} // namespace vasum
