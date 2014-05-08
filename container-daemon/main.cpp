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
 * @brief   Main file for the Security Containers Daemon
 */

// Always log to console in DEBUG mode
#if !defined(LOG_TO_CONSOLE) && !defined(NDEBUG)
#define LOG_TO_CONSOLE
#endif

#include "log/logger.hpp"
#include "log/backend-stderr.hpp"
#include "log/backend-journal.hpp"
#include "utils/typeinfo.hpp"

#include "exception.hpp"
#include "runner.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <iostream>

using namespace security_containers::log;
using namespace security_containers;

namespace po = boost::program_options;


namespace {

const std::string PROGRAM_NAME_AND_VERSION =
    "Security Containers Containers Daemon " PROGRAM_VERSION;

/**
 * TODO: This is a copied function, move to common/log
 * Resolve if given log severity level is valid
 *
 * @param s     log severity level
 * @return      LogLevel when valid,
 *              otherwise exception po::validation_error::invalid_option_value thrown
 */
LogLevel validateLogLevel(const std::string& s)
{
    std::string s_capitalized = boost::to_upper_copy(s);

    if (s_capitalized == "ERROR") {
        return LogLevel::ERROR;
    } else if (s_capitalized == "WARN") {
        return LogLevel::WARN;
    } else if (s_capitalized == "INFO") {
        return LogLevel::INFO;
    } else if (s_capitalized == "DEBUG") {
        return LogLevel::DEBUG;
    } else if (s_capitalized == "TRACE") {
        return LogLevel::TRACE;
    } else {
        throw po::validation_error(po::validation_error::invalid_option_value);
    }
}

} // namespace


int main(int argc, char* argv[])
{
    std::string configPath ;

    try {
        po::options_description desc("Allowed options");

        desc.add_options()
        ("help,h", "print this help")
        ("version,v", "show application version")
        ("log-level,l", po::value<std::string>()->default_value("DEBUG"), "set log level")
        ;

        po::variables_map vm;
        po::basic_parsed_options< char > parsed =
            po::command_line_parser(argc, argv).options(desc).allow_unregistered().run();

        std::vector<std::string> unrecognized_options =
            po::collect_unrecognized(parsed.options, po::include_positional);

        if (!unrecognized_options.empty()) {
            std::cerr << "Unrecognized options: ";

            for (auto& uo : unrecognized_options) {
                std::cerr << ' ' << uo;
            }

            std::cerr << std::endl << std::endl;
            std::cerr << desc << std::endl;

            return 1;
        }

        po::store(parsed, vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        } else if (vm.count("version")) {
            std::cout << PROGRAM_NAME_AND_VERSION << std::endl;
            return 0;
        }

        LogLevel level = validateLogLevel(vm["log-level"].as<std::string>());
        Logger::setLogLevel(level);
#ifdef LOG_TO_CONSOLE
        Logger::setLogBackend(new StderrBackend());
#else
        Logger::setLogBackend(new SystemdJournalBackend());
#endif


    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    try {
        container_daemon::Runner daemon;
        daemon.run();

    } catch (std::exception& e) {
        LOGE("Unexpected: " << utils::getTypeName(e) << ": " << e.what());
        return 1;
    }

    return 0;
}

