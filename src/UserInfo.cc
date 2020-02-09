/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#include "StratumServer.h"
#include "UserInfo.h"

//////////////////////////////////// UserInfo /////////////////////////////////
UserInfo::UserInfo(StratumServer *server, const libconfig::Config &config)
  : running_(true)
  , caseInsensitive_(true)
  , stripUserSuffix_(false)
  , userSuffixSeparator_("_")
  , server_(server)
  , enableAutoReg_(false)
  , autoRegMaxPendingUsers_(50) {
  // optional
  config.lookupValue("users.case_insensitive", caseInsensitive_);
  config.lookupValue("users.strip_user_suffix", stripUserSuffix_);
  config.lookupValue("users.user_suffix_separator", userSuffixSeparator_);
  config.lookupValue("users.enable_auto_reg", enableAutoReg_);
  config.lookupValue(
      "users.auto_reg_max_pending_users", autoRegMaxPendingUsers_);
  config.lookupValue("users.zookeeper_auto_reg_watch_dir", zkAutoRegWatchDir_);
  config.lookupValue(
      "users.namechains_check_interval", nameChainsCheckIntervalSeconds_);

  LOG(INFO) << "UserInfo: user name will be case "
            << (caseInsensitive_ ? "insensitive" : "sensitive");

  if (enableAutoReg_) {
    LOG(INFO) << "UserInfo: auto register enabled";

    if (zkAutoRegWatchDir_[zkAutoRegWatchDir_.size() - 1] != '/') {
      zkAutoRegWatchDir_ += '/';
    }

    if (!zk_) {
      zk_ = server->getZookeeper(config);
    }
  }

  if (stripUserSuffix_) {
    if (userSuffixSeparator_.empty()) {
      LOG(FATAL) << "users.strip_user_suffix enabled but "
                    "users.user_suffix_separator is empty!";
    }

    LOG(INFO) << "UserInfo: suffix " << userSuffixSeparator_
              << "* will be stripped from user name";
  }

  auto addChainVars = [&](const string &apiUrl) {
    chains_.push_back({
        apiUrl,
        std::make_unique<std::shared_timed_mutex>(), // rwlock_
        {}, // nameIds_
        0, // lastMaxUserId_
        0, // lastTime_
        {} // threadUpdate_
    });
  };

  bool multiChains = false;
  config.lookupValue("sserver.multi_chains", multiChains);

  if (multiChains) {
    const Setting &chains = config.lookup("chains");
    for (int i = 0; i < chains.getLength(); i++) {
      addChainVars(chains[i].lookup("users_list_id_api_url"));
    }
    if (chains_.empty()) {
      LOG(FATAL) << "sserver.multi_chains enabled but chains is empty!";
    }
#ifndef USE_API_SWTICH_MULTICHAIN
    if (chains_.size() > 1) {
      if (!zk_) {
        zk_ = server->getZookeeper(config);
      }
      zkUserChainMapDir_ =
          config.lookup("users.zookeeper_userchain_map").c_str();
      if (zkUserChainMapDir_.empty()) {
        LOG(FATAL) << "users.zookeeper_userchain_map cannot be empty!";
      }
      if (zkUserChainMapDir_[zkUserChainMapDir_.size() - 1] != '/') {
        zkUserChainMapDir_ += '/';
      }
      setZkReconnectHandle();
    }
#endif
  } else {
    // required (exception will be threw if inexists)
    addChainVars(config.lookup("users.list_id_api_url"));
  }
  mainChain_ = {
        config.lookup("users.list_id_api_url"),
        std::make_unique<std::shared_timed_mutex>(), // rwlock_
        {}, // nameIds_
        0, // lastMaxUserId_
        0, // lastTime_
        {} // threadUpdate_
    };
}

UserInfo::~UserInfo() {
  stop();

#ifdef USE_API_SWTICH_MULTICHAIN
  if (mainChain_.threadUpdate_.joinable())
    mainChain_.threadUpdate_.join();
#else
  for (ChainVars &chain : chains_) {
    if (chain.threadUpdate_.joinable())
      chain.threadUpdate_.join();
  }

  if (nameChainsCheckingThread_.joinable()) {
    nameChainsCheckingThread_.join();
  }
#endif
}

void UserInfo::stop() {
  if (!running_)
    return;

  running_ = false;
}

void UserInfo::regularUserName(string &userName) {
  if (caseInsensitive_) {
    std::transform(
        userName.begin(), userName.end(), userName.begin(), ::tolower);
  }
  if (stripUserSuffix_) {
    size_t pos = userName.rfind(userSuffixSeparator_);
    if (pos != userName.npos) {
      userName = userName.substr(0, pos);
      DLOG(INFO) << "User Suffix Stripped: " << userName;
    }
  }
}

bool UserInfo::getChainIdFromZookeeper(
    const string &userName, size_t &chainId) {
  try {
    // Prevent buffer overflow attacks on zookeeper
    if (userName.size() > 200) {
      LOG(WARNING) << "UserInfo::getChainIdFromZookeeper(): too long username: "
                   << userName;
      return false;
    }

    string nodePath = zkUserChainMapDir_ + userName;
    string chainName;
    if (zk_->getValueW(nodePath, chainName, 64, handleSwitchChainEvent, this)) {
      DLOG(INFO) << "zk userchain map: " << userName << " : " << chainName;

      // auto switch chain
      if (server_->management().autoSwitchChainEnabled() &&
          chainName == AUTO_CHAIN_NAME) {
        chainId = server_->management().currentAutoChainId();
        // add to cache
        std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
        nameChains_[userName] = {chainId, true};
        return true;
      }

      for (chainId = 0; chainId < server_->chains_.size(); chainId++) {
        if (chainName == server_->chains_[chainId].name_) {
          bool found = false;
          {
            ChainVars &chain = chains_[chainId];
            std::shared_lock<std::shared_timed_mutex> l{*chain.nameIdlock_};
            auto itr = chain.nameIds_.find(userName);
            found = itr != chain.nameIds_.end();
          }
          if (found) {
            // add to cache
            std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
            nameChains_[userName] = {chainId, false};
            return true;
          } else {
            LOG(ERROR) << "Userlist for chain " << server_->chainName(chainId)
                       << " missing user " << userName;
            return false;
          }
        }
      }
      // cannot find the chain, warning and ignore it
      LOG(WARNING)
          << "UserInfo::getChainIdFromZookeeper(): Unknown chain name '"
          << chainName << "' in zookeeper node '" << nodePath << "'.";
    } else {
      LOG(INFO) << "cannot find mining chain in zookeeper, user name: "
                << userName << " (" << nodePath << ")";
    }
  } catch (const std::exception &ex) {
    LOG(ERROR)
        << "UserInfo::getChainIdFromZookeeper(): zk_->getValueW() failed: "
        << ex.what();
  } catch (...) {
    LOG(ERROR) << "UserInfo::getChainIdFromZookeeper(): unknown exception";
  }
  return false;
}

void UserInfo::setZkReconnectHandle() {
  zk_->registerReconnectHandle([this]() {
    LOG(WARNING)
        << "zookeeper reconnected, trigger SwitchChainEvent for all users";

    // Chain switching while holding a lock can result in a deadlock.
    // So release the lock immediately after copying.
    nameChainlock_.lock_shared();
    auto nameChains = nameChains_;
    nameChainlock_.unlock_shared();

    // Check the current chain of all users
    for (auto item : nameChains) {
      handleSwitchChainEvent(item.first);
    }
  });
}

void UserInfo::handleSwitchChainEvent(
    zhandle_t *, int type, int state, const char *path, void *pUserInfo) {
  if (path == nullptr || pUserInfo == nullptr) {
    return;
  }

  DLOG(INFO) << "UserInfo::handleSwitchChainEvent: type:" << type
             << ", state:" << state << ", path:" << path;

  UserInfo *userInfo = (UserInfo *)pUserInfo;
  string nodePath(path);

  if (static_cast<ssize_t>(nodePath.size()) -
          static_cast<ssize_t>(userInfo->zkUserChainMapDir_.size()) <
      1) {
    return;
  }

  string userName = nodePath.substr(userInfo->zkUserChainMapDir_.size());
  userInfo->handleSwitchChainEvent(userName);
}

void UserInfo::handleSwitchChainEvent(const string &userName) {

  // lookup cache
  std::shared_lock<std::shared_timed_mutex> l{nameChainlock_};
  auto itr = nameChains_.find(userName);
  if (itr == nameChains_.end()) {
    LOG(INFO) << "No workers of user " << userName
              << " online, switching request will be ignored";
    return;
  }
  size_t currentChainId = itr->second.chainId_;
  bool oldAutoChainStatus = itr->second.autoSwitchChain_;
  l.unlock();

  size_t newChainId = 0;
  if (!getChainIdFromZookeeper(userName, newChainId)) {
    LOG(ERROR) << "UserInfo::handleZookeeperEvent(): cannot get chain id from "
                  "zookeeper, switching request will be ignored";
    return;
  }
  bool newAutoChainStatus = itr->second.autoSwitchChain_;
  if (oldAutoChainStatus != newAutoChainStatus) {
    LOG(INFO) << "[auto chain] "
              << (newAutoChainStatus ? "enabled" : "disabled")
              << " auto switch chain, user: " << userName;
  }
  if (currentChainId == newChainId) {
    LOG(INFO) << (newAutoChainStatus ? "[auto chain] " : "")
              << "Ignore empty switching request for user '" << userName
              << "': " << server_->chainName(currentChainId) << " -> "
              << server_->chainName(newChainId);
    return;
  }

  const int32_t newUserId = getUserId(newChainId, userName);
  if (newUserId <= 0) {
    LOG(INFO) << (newAutoChainStatus ? "[auto chain] " : "")
              << "Ignore switching request: cannot find user id, chainId: "
              << newChainId << ", userName: " << userName;
    return;
  }

  server_->dispatch(
      [this, userName, currentChainId, newChainId, newAutoChainStatus]() {
        size_t onlineSessions = server_->switchChain(userName, newChainId);

        if (onlineSessions == 0) {
          LOG(INFO) << (newAutoChainStatus ? "[auto chain] " : "")
                    << "No workers of user " << userName
                    << " online, subsequent switching request will be ignored";
          // clear cache
          std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
          auto itr = nameChains_.find(userName);
          if (itr != nameChains_.end()) {
            nameChains_.erase(itr);
          }
        }

        LOG(INFO) << (newAutoChainStatus ? "[auto chain] " : "") << "User '"
                  << userName << "' (" << onlineSessions
                  << " miners) switched chain: "
                  << server_->chainName(currentChainId) << " -> "
                  << server_->chainName(newChainId);
      });
}

void UserInfo::autoSwitchChain(
    size_t newChainId,
    std::function<
        void(size_t oldChain, size_t newChain, size_t users, size_t miners)>
        callback) {
  // update caches
  size_t oldChainId = 0;
  size_t users = 0;
  {
    std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
    for (auto &itr : nameChains_) {
      if (itr.second.autoSwitchChain_) {
        if (users == 0) {
          oldChainId = itr.second.chainId_;
        }
        itr.second.chainId_ = newChainId;
        users++;
      }
    }
  }

  // do the switch
  server_->dispatch([this, oldChainId, newChainId, users, callback]() {
    size_t switchedSessions = server_->autoSwitchChain(newChainId);

    LOG(INFO) << "[auto chain] " << users << " users (" << switchedSessions
              << " miners) switched chain: " << server_->chainName(oldChainId)
              << " -> " << server_->chainName(newChainId);

    callback(oldChainId, newChainId, users, switchedSessions);

    if (switchedSessions > 0) {
      server_->chains_[newChainId].jobRepository_->sendLatestMiningNotify();
    }
  });
}

//// new api
void UserInfo::handleSwitchChainEvent(const string &userName, const int32_t userId, const size_t currentChainId, const size_t newChainId) {
  if (currentChainId == newChainId) {
    LOG(INFO) << "Ignore empty switching request for user '" << userName
              << "': " << server_->chainName(currentChainId) << " -> "
              << server_->chainName(newChainId);
    return;
  }

  if (userId <= 0) {
    LOG(INFO) << "Ignore switching request: cannot find user id, chainId: "
              << newChainId << ", userName: " << userName;
    return;
  }

  server_->dispatch([this, userName, currentChainId, newChainId]() {
    size_t onlineSessions = server_->switchChain(userName, newChainId);

    if (onlineSessions == 0) {
      LOG(INFO) << "No workers of user " << userName
                << " online, subsequent switching request will be ignored";
      // clear cache
      std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
      auto itr = nameChains_.find(userName);
      if (itr != nameChains_.end()) {
        nameChains_.erase(itr);
      }
    }

    LOG(INFO) << "User '" << userName << "' (" << onlineSessions
              << " miners) switched chain: "
              << server_->chainName(currentChainId) << " -> "
              << server_->chainName(newChainId);
  });
}

bool UserInfo::getChainIdByName_ApiSwitch(const string &chainName, size_t &chainId) {
  if (server_->chains_.size() == 1) {
    chainId = 0;
    return true;
  }
  for (size_t cId = 0; cId < server_->chains_.size(); cId++) {
    if (chainName == server_->chainName(cId)) {
      chainId = cId;
      return true;
    }
  }
  return false;
}

bool UserInfo::getChainId_ApiSwitch(const string &userName, size_t &chainId) {
  if (server_->chains_.size() == 1) {
    chainId = 0;
    return true;
  }

  {
    // lookup name -> chain cache map
    std::shared_lock<std::shared_timed_mutex> l{nameChainlock_};
    auto itr = nameChains_.find(userName);
    if (itr != nameChains_.end()) {
      chainId = itr->second.chainId_;
      return true;
    }
  }

  // Not found in all chains
  return false;
}

bool UserInfo::getChainId(const string &userName, size_t &chainId) {
#ifdef USE_API_SWTICH_MULTICHAIN
  // use new version.
  return getChainId_ApiSwitch(userName, chainId);
#endif
  if (chains_.size() == 1) {
    chainId = 0;
    return true;
  }

  {
    // lookup name -> chain cache map
    std::shared_lock<std::shared_timed_mutex> l{nameChainlock_};
    auto itr = nameChains_.find(userName);
    if (itr != nameChains_.end()) {
      chainId = itr->second.chainId_;
      return true;
    }
  }

  // lookup zookeeper
  if (getChainIdFromZookeeper(userName, chainId)) {
    return true;
  }

  // lookup each chain
  // The first chain's id that find the user will be returned.
  for (chainId = 0; chainId < chains_.size(); chainId++) {
    ChainVars &chain = chains_[chainId];
    std::shared_lock<std::shared_timed_mutex> l{*chain.nameIdlock_};
    auto itr = chain.nameIds_.find(userName);
    if (itr != chain.nameIds_.end()) {
      // chainId has been assigned to the correct value
      DLOG(INFO) << "userName: " << userName << ", chainId: " << chainId;
      // add to cache
      std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
      nameChains_[userName] = {chainId, false};
      return true;
    }
  }

  // Not found in all chains
  return false;
}

int32_t UserInfo::getUserId(size_t chainId, const string &userName) {
#ifdef USE_API_SWTICH_MULTICHAIN
  ChainVars &chain = mainChain_;
#else
  ChainVars &chain = chains_[chainId];
#endif

  std::shared_lock<std::shared_timed_mutex> l{*chain.nameIdlock_};
  auto itr = chain.nameIds_.find(userName);
  if (itr != chain.nameIds_.end()) {
    return itr->second;
  }
  return 0; // not found
}

int32_t UserInfo::incrementalUpdateUsers(size_t chainId) {
  ChainVars &chain = chains_[chainId];

  //
  // WARNING: The API is incremental update, we use `?last_id=` to make sure
  //          always get the new data. Make sure you have use `last_id` in API.
  //
  const string url =
      Strings::Format("%s?last_id=%d", chain.apiUrl_, chain.lastMaxUserId_);
  string resp;
  if (!httpGET(url.c_str(), resp, 10000 /* timeout ms */)) {
    LOG(ERROR) << "http get request user list fail, url: " << url;
    return -1;
  }

  JsonNode r;
  if (!JsonNode::parse(resp.c_str(), resp.c_str() + resp.length(), r)) {
    LOG(ERROR) << "decode json fail, json: " << resp;
    return -1;
  }
  if (r["data"].type() == Utilities::JS::type::Undefined) {
    LOG(ERROR) << "invalid data, should key->value, type: "
               << (int)r["data"].type();
    return -1;
  }
  auto vUser = r["data"].children();
  if (vUser->size() == 0) {
    return 0;
  }

  {
    std::unique_lock<std::shared_timed_mutex> l{*chain.nameIdlock_};
    for (const auto &itr : *vUser) {
      string userName = itr.key();
      regularUserName(userName);

      const int32_t userId = itr.int32();
      if (userId > chain.lastMaxUserId_) {
        chain.lastMaxUserId_ = userId;
      }

      chain.nameIds_.insert(std::make_pair(userName, userId));
    }
  }

  return vUser->size();
}

// new version for update users via http api.
int32_t UserInfo::incrementalUpdateUsers_ApiSwitch(size_t chainId) {
  ChainVars &chain = mainChain_;

  //
  // WARNING: The API is incremental update, we use `?last_id=*&last_time=*` to
  // make sure
  //          always get the new data. Make sure you have use `last_id` and
  //          `last_time` in API.
  //
  const string url = Strings::Format(
      "%s?algo=sha256d&last_id=%d&last_time=%d",
      chain.apiUrl_,
      chain.lastMaxUserId_,
      chain.lastTime_);
  string resp;
  if (!httpGET(url.c_str(), resp, 10000 /* timeout ms */)) {
    LOG(ERROR) << "http get request user list fail, url: " << url;
    return -1;
  }

  JsonNode r;
  if (!JsonNode::parse(resp.c_str(), resp.c_str() + resp.length(), r)) {
    LOG(ERROR) << "decode json fail, json: " << resp;
    return -1;
  }
  if (r["data"].type() == Utilities::JS::type::Undefined) {
    LOG(ERROR) << "invalid data, should key->value, type: "
               << (int)r["data"].type();
    return -1;
  }

  auto vUser = r["data"].children();
  if (vUser->size() == 0) {
    return 0;
  }
  chain.lastTime_ = r["time"].int64();

  if (chain.lastTime_ == 0) {
    // old version json.
    std::unique_lock<std::shared_timed_mutex> l{*chain.nameIdlock_};
    for (const auto &itr : *vUser) {
      string userName = itr.key();
      regularUserName(userName);

      const int32_t userId = itr.int32();
      if (userId > chain.lastMaxUserId_) {
        chain.lastMaxUserId_ = userId;
      }

      chain.nameIds_.insert(std::make_pair(userName, userId));
    }
    return vUser->size();
  }

  std::unique_lock<std::shared_timed_mutex> l{*chain.nameIdlock_};
  for (JsonNode &itr : *vUser) {

    string userName = itr.key();
    regularUserName(userName);

    if (itr.type() != Utilities::JS::type::Obj) {
      LOG(ERROR) << "invalid data, should key  - value" << std::endl;
      return -1;
    }

    const int32_t userId = itr["puid"].int32();
    string chainName = itr["chain"].str();

    if (userId > chain.lastMaxUserId_) {
      chain.lastMaxUserId_ = userId;
    }
    chain.nameIds_.insert(std::make_pair(userName, userId));

    size_t currChainId;
    size_t newChainId;
    if (!getChainIdByName_ApiSwitch(chainName, newChainId))
      continue;

    if (getChainId_ApiSwitch(userName, currChainId)) {
      // exists chainid, means switch chain.
      if (currChainId != newChainId) {
        std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
        nameChains_[userName] = {newChainId, false};
        handleSwitchChainEvent(userName, userId, currChainId, newChainId);
        LOG(INFO) << "update user id: " << userId << ", user name: " << userName << ", chain name: " << chainName << ", chain id: " << newChainId;
      }
    } else {
        std::unique_lock<std::shared_timed_mutex> l{nameChainlock_};
        nameChains_[userName] = {newChainId, false};
        LOG(INFO) << "insert user id: " << userId << ", user name: " << userName << ", chain name: " << chainName << ", chain id: " << newChainId << " map size: " << nameChains_.size();
    }
  }

  return vUser->size();
}

void UserInfo::runThreadUpdate(size_t chainId) {
  //
  // get all user list, incremental update model.
  //
  // We use `offset` in incrementalUpdateUsers(), will keep update uitl no more
  // new users. Most of http API have timeout limit, so can't return lots of
  // data in one request.
  //

  const time_t updateInterval = 10; // seconds
  time_t lastUpdateTime = 0;

  while (running_) {
    if (lastUpdateTime + updateInterval > time(nullptr)) {
      std::this_thread::sleep_for(500ms);
      continue;
    }

#ifdef USE_API_SWTICH_MULTICHAIN
    int32_t res = incrementalUpdateUsers_ApiSwitch(chainId);
#else
    int32_t res = incrementalUpdateUsers(chainId);
#endif
    lastUpdateTime = time(nullptr);

    if (res > 0) {
#ifdef USE_API_SWTICH_MULTICHAIN
      LOG(INFO) << "chain default multichain update users count: " << res;
#else
      LOG(INFO) << "chain " << server_->chainName(chainId)
                << " update users count: " << res;
#endif
    }
  }
}

bool UserInfo::setupThreads() {
#ifdef USE_API_SWTICH_MULTICHAIN
  ChainVars &chain = mainChain_;

  chain.threadUpdate_ = std::thread(&UserInfo::runThreadUpdate, this, 0);
#else
  for (size_t chainId = 0; chainId < chains_.size(); chainId++) {
    ChainVars &chain = chains_[chainId];

    chain.threadUpdate_ =
        std::thread(&UserInfo::runThreadUpdate, this, chainId);
  }

  if (chains_.size() > 1) {
    nameChainsCheckingThread_ =
        std::thread(std::bind(&UserInfo::checkNameChains, this));
  }
#endif

  return true;
}

bool /*isInterrupted*/ UserInfo::interruptibleSleep(time_t seconds) {
  const time_t sleepEnd = time(nullptr) + seconds;
  while (time(nullptr) < sleepEnd) {
    if (!running_) {
      return true;
    }
    std::this_thread::sleep_for(1s);
  }
  return false;
}

void UserInfo::checkNameChains() {
  LOG(INFO) << "UserInfo::checkNameChains running...";

  if (interruptibleSleep(nameChainsCheckIntervalSeconds_))
    return;

  while (running_) {
    // Chain switching while holding a lock can result in a deadlock.
    // So release the lock immediately after copying.
    nameChainlock_.lock_shared();
    auto nameChains = nameChains_;
    nameChainlock_.unlock_shared();

    if (nameChains.empty()) {
      if (interruptibleSleep(nameChainsCheckIntervalSeconds_))
        return;
      continue;
    }

    time_t eachUserSleepMillisecond =
        nameChainsCheckIntervalSeconds_ * 1000 / nameChains.size();
    if (eachUserSleepMillisecond == 0)
      eachUserSleepMillisecond = 1;
    LOG(INFO) << "UserInfo::checkNameChains checking, each user sleep "
              << eachUserSleepMillisecond << "ms";

    for (auto itr : nameChains) {
      if (!running_) {
        return;
      }

      try {
        const string &userName = itr.first;
        const auto &chainInfo = itr.second;
        const string &chainName = server_->chains_[chainInfo.chainId_].name_;

        string nodePath = zkUserChainMapDir_ + userName;
        string newChainName = zk_->getValue(nodePath, 64);

        if (server_->management().autoSwitchChainEnabled() &&
            newChainName == AUTO_CHAIN_NAME) {
          if (chainInfo.autoSwitchChain_ &&
              chainInfo.chainId_ ==
                  server_->management().currentAutoChainId()) {
            DLOG(INFO) << "[auto chain] User does not switch chains, user: "
                       << userName
                       << ", chain: " << server_->chainName(chainInfo.chainId_);
          } else {
            LOG(INFO) << "[auto chain] User switched the chain, user: "
                      << userName
                      << ", chains: " << server_->chainName(chainInfo.chainId_)
                      << " -> "
                      << server_->chainName(
                             server_->management().currentAutoChainId());
            handleSwitchChainEvent(userName);
          }
        } else if (chainName == newChainName) {
          DLOG(INFO) << "User does not switch chains, user: " << userName
                     << ", chain: " << chainName;
        } else {
          LOG(INFO) << "User switched the chain, user: " << userName
                    << ", chains: " << chainName << " -> " << newChainName;
          handleSwitchChainEvent(userName);
        }

      } catch (const std::exception &ex) {
        LOG(ERROR) << "UserInfo::checkNameChains(): zk_->getValue() failed: "
                   << ex.what();
      } catch (...) {
        LOG(ERROR) << "UserInfo::checkNameChains(): unknown exception";
      }

      if (eachUserSleepMillisecond > 5000) {
        if (interruptibleSleep(eachUserSleepMillisecond / 1000))
          return;
      } else {
        std::this_thread::sleep_for(eachUserSleepMillisecond * 1ms);
      }
    }
  }
}

void UserInfo::handleAutoRegEvent(
    zhandle_t *zh, int type, int state, const char *path, void *pUserInfo) {
  if (path == nullptr || pUserInfo == nullptr) {
    return;
  }

  DLOG(INFO) << "UserInfo::handleAutoRegEvent: type:" << type
             << ", state:" << state << ", path:" << path;

  UserInfo *userInfo = (UserInfo *)pUserInfo;
  string nodePath(path);

  if (static_cast<ssize_t>(nodePath.size()) -
          static_cast<ssize_t>(userInfo->zkUserChainMapDir_.size()) <
      1) {
    return;
  }

  string userName = nodePath.substr(userInfo->zkAutoRegWatchDir_.size());

  {
    ScopeLock lock(userInfo->autoRegPendingUsersLock_);
    userInfo->autoRegPendingUsers_.erase(userName);
  }

  userInfo->server_->dispatch([userInfo, userName]() {
    size_t sessions = userInfo->server_->autoRegCallback(userName);

    LOG(INFO) << "Auto Reg: User '" << userName << "' (" << sessions
              << " miners online) registered";
  });
}

bool UserInfo::tryAutoReg(
    string userName, uint32_t sessionId, string fullWorkerName) {
  try {
    fullWorkerName = filterWorkerName(fullWorkerName);
    userName = filterWorkerName(userName);

    {
      ScopeLock lock(autoRegPendingUsersLock_);
      if (autoRegPendingUsers_.find(userName) != autoRegPendingUsers_.end()) {
        return true;
      }
      if (autoRegPendingUsers_.size() >= autoRegMaxPendingUsers_) {
        LOG(INFO) << "UserInfo: too many pending registing request, user: "
                  << userName;
        return false;
      }
      autoRegPendingUsers_.insert(userName);
    }

    string userInfo = Strings::Format(
        "{"
        "\"SessionID\":%u,"
        "\"Worker\":\"%s\""
        "}",
        sessionId,
        fullWorkerName);

    string zkPath = zkAutoRegWatchDir_ + userName;
    zk_->createNode(zkPath, userInfo);
    zk_->watchNode(zkPath, handleAutoRegEvent, this);
    return true;
  } catch (const std::exception &ex) {
    LOG(ERROR) << "UserInfo::tryAutoReg() exception: " << ex.what();
  } catch (...) {
    LOG(ERROR) << "UserInfo::tryAutoReg(): unknown exception";
  }

  ScopeLock lock(autoRegPendingUsersLock_);
  autoRegPendingUsers_.erase(userName);
  return false;
}

bool UserInfo::userAutoSwitchChainEnabled(const string &userName) {
  std::shared_lock<std::shared_timed_mutex> l{nameChainlock_};
  return nameChains_[userName].autoSwitchChain_;
}
