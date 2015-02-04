/*
*  Copyright (c) 2015 Samsung Electronics Co., Ltd All Rights Reserved
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
 * @brief   Types definitions and helper functions
 */

#include "config.hpp"

#include "ipc/types.hpp"
#include "logger/logger.hpp"

#include <atomic>

namespace vasum {
namespace ipc {

namespace {
std::atomic<MessageID> gLastMessageID(0);
std::atomic<PeerID> gLastPeerID(0);
} // namespace

MessageID getNextMessageID()
{
    return ++gLastMessageID;
}

PeerID getNextPeerID()
{
    return ++gLastPeerID;
}


} // namespace ipc
} // namespace vasum
