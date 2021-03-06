#include <cstdio>
#include <cstring>
#include <memory>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <grp.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <json/json.h>

#include "logger.h"
#include "zkmgr.h"

#define ERRBUF_MAX      1024
#define ZKRETRY_MAX     100
#define ZKRETRY_SLEEP   500  // ms
#define RENV_ITEM_MAX   5
#define RENV_BUFFER_LEN PIPE_BUF * 6


extern char **environ;
static std::string ENV_STICK;
#define dump_stick(id) ENV_STICK.assign("DCRON_TEST_STICK=").append((id))

static pthread_mutex_t PTHREAD_MUTEX = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  PTHREAD_COND  = PTHREAD_COND_INITIALIZER;

inline void millisleep(int milli)
{
  struct timespec spec = { 0, milli * 1000 * 1000 };
  nanosleep(&spec, 0);
}

/*
static void log_callback(const char *message)
{
  log_error(0, "zk error %s", message);
}
*/

void (*log_callback_fn)(const char *message);

// static char ACL_SCHEMA[] = "digest";
// static char ACL_ID[]     = "dcron:dcron";
static char ACL_SCHEMA[] = "world";
static char ACL_ID[]     = "anyone";

static struct ACL _DCRON_ALL_ACL[] = {{ZOO_PERM_ALL, {ACL_SCHEMA, ACL_ID}}};
static struct ACL_vector ZOO_DCRON_ALL_ACL = {1, _DCRON_ALL_ACL};

inline bool createNodeIfNotExist(zhandle_t *zh, const char *node, char *errbuf)
{
  for (int i = 0; /**/; /**/) {
    int rc = zoo_create(zh, node, 0, -1, &ZOO_DCRON_ALL_ACL, 0, 0, 0);
    if (rc == ZOK || rc == ZNODEEXISTS) {
      return true;
    } else if (rc != ZCONNECTIONLOSS) {
      snprintf(errbuf, ERRBUF_MAX, "zoo_create %s error, %s", node, zerror(rc));
      return false;
    }
    if (++i < ZKRETRY_MAX) millisleep(ZKRETRY_SLEEP);
  }
  return false;
}

static bool createStickFile(const std::string &libdir, const char *name)
{
  const char *dot = strrchr(name, '.');
  if (!dot) return false;

  std::string stickFile = libdir + "/" + std::string(name, dot - name) + ".stick";
  FILE *fp = fopen(stickFile.c_str(), "w");
  if (!fp) return false;

  fprintf(fp, "%ld", (long) time(0));
  fclose(fp);
  return true;
}

static bool getStickFile(const std::string &libdir, const char *name, int stick)
{
  const char *dot = strrchr(name, '.');
  assert(dot);

  std::string stickFile = libdir + "/" + std::string(name, dot - name) + ".stick";
  FILE *fp = fopen(stickFile.c_str(), "r");
  if (!fp) return false;

  bool rc = false;
  long lastExecTime = 0;
  if (fscanf(fp, "%ld", &lastExecTime) == 1 && time(0) - lastExecTime < stick) {
    rc = true;
  }
  fclose(fp);
  unlink(stickFile.c_str());
  return rc;
}

const char *ZkMgr::statusToString(NodeStatus status)
{
  switch (status) {
  case MASTER:  return "master";
  case SLAVE:   return "slave";
  case OUT:     return "out";
  case ZKOK:    return "zkok";
  case ZKAGAIN: return "zkagain";
  case ZKFATAL: return "zkfatal";
  default: return "unknow";
  }
}

/* x.y.<taskid> -> /x/y/<taskid>
 * - /x/y/llap  persistent data across sessions
 * - <taskid>/master   EPHEMERAL
 * - <taskid>/workers
 * - <taskid>/status
 * - <taskid>/result   ZOO_SEQUENCE
 */
bool ZkMgr::createWorkDir(char *errbuf)
{
  taskPath_.assign(1, '/');
  for (const char *ptr = cnf_->name(); *ptr; ++ptr) {
    if (*ptr == '.') taskPath_.append(1, '/');
    else taskPath_.append(1, *ptr);
  }

  for (size_t i = 2; i < taskPath_.size(); ++i) {
    if (taskPath_[i] != '/') continue;

    taskPath_[i] = '\0';
    bool rc = createNodeIfNotExist(zh_, taskPath_.data(), errbuf);
    taskPath_[i] = '/';

    if (!rc) return rc;
  }

  bool rc = createNodeIfNotExist(zh_, taskPath_.data(), errbuf);
  if (!rc) return rc;

  masterNode_  = taskPath_ + "/master";
  workersNode_ = taskPath_ + "/workers";
  statusNode_  = taskPath_ + "/status";
  resultNode_  = taskPath_ + "/result";

  size_t slash = taskPath_.rfind('/');
  assert(slash != 1 && slash != std::string::npos);

  llapNode_ = taskPath_.substr(0, slash) + "/llap";

  if (!createNodeIfNotExist(zh_, workersNode_.c_str(), errbuf)) return false;
  if (!createNodeIfNotExist(zh_, llapNode_.c_str(), errbuf)) return false;
  return true;
}

ZkMgr::NodeStatus ZkMgr::joinWorkers(bool master, char *errbuf)
{
  char buffer[1024];
  struct Stat stat;

  for (int i = 0; /**/; /**/) {
    do {
      int bufferLen = 1024;
      int rc = zoo_get(zh_, workersNode_.c_str(), 0, buffer, &bufferLen, &stat);
      if (rc == ZCONNECTIONLOSS) break;

      if (rc != ZOK) {
        if (errbuf) snprintf(errbuf, ERRBUF_MAX, "zoo_get %s error, %s", workersNode_.c_str(), zerror(rc));
        else log_fatal(0, "zoo_get %s error, %s", workersNode_.c_str(), zerror(rc));

        return ZKFATAL;
      }

      Json::Value array;
      if (bufferLen <= 0) {
        array = Json::Value(Json::arrayValue);
      } else {
        Json::Reader reader;
        if (!reader.parse(buffer, buffer + bufferLen, array)) {
          if (errbuf) snprintf(errbuf, ERRBUF_MAX, "%s content %s error", workersNode_.c_str(), buffer);
          else log_fatal(0, "%s content %s error", workersNode_.c_str(), buffer);

          return ZKFATAL;
        }
      }

      if (!master && array.size() >= cnf_->maxRetry()) return OUT;
      array.append(cnf_->id());

      std::string json = Json::FastWriter().write(array);
      if (json[json.size()-1] == '\n') json.resize(json.size()-1);

      if (errbuf) fprintf(stderr, "zoo_set workers %s %s\n", workersNode_.c_str(), json.c_str());
      else log_info(0, "zoo_set workers %s %s", workersNode_.c_str(), json.c_str());

      rc = zoo_set(zh_, workersNode_.c_str(), json.c_str(), json.size(), stat.version);
      if (rc == ZOK) {
        return master ? MASTER : SLAVE;
      } else if (rc == ZCONNECTIONLOSS) {
        break;
      } else if (rc != ZBADVERSION) {
        if (errbuf) snprintf(errbuf, ERRBUF_MAX, "zoo_set %s error, %s", workersNode_.c_str(), zerror(rc));
        else log_fatal(0, "zoo_set %s error, %s", workersNode_.c_str(), zerror(rc));

        return ZKFATAL;
      }
    } while (true);

    if (++i < ZKRETRY_MAX) millisleep(ZKRETRY_SLEEP);
  }
  return ZKFATAL;
}

ZkMgr::NodeStatus ZkMgr::competeMaster(bool first, char *errbuf)
{
  do {
    int rc;
    if (cnf_->testConnectionLossWhenCompeteMasterFailure()) {
      rc = ZCONNECTIONLOSS;
    } else {
      rc = zoo_create(zh_, masterNode_.c_str(), cnf_->id(), strlen(cnf_->id()), &ZOO_DCRON_ALL_ACL,
                      ZOO_EPHEMERAL, 0, 0);
      if (cnf_->testConnectionLossWhenCompeteMasterSuccess()) rc = ZCONNECTIONLOSS;
    }

    if (rc == ZOK) {
      return first ? joinWorkers(true, errbuf) : MASTER;
    } else if (rc == ZNODEEXISTS) {
      return first ? joinWorkers(false, errbuf) : SLAVE;
    } else if (rc == ZCONNECTIONLOSS) {
      int bufferLen = RENV_BUFFER_LEN;
      std::auto_ptr<char> buffer(new char[RENV_BUFFER_LEN]);

      for (int i = 0; /**/; /**/) {
        rc = zoo_get(zh_, masterNode_.c_str(), 0, buffer.get(), &bufferLen, 0);
        if (rc == ZOK) {
          if (strncmp(buffer.get(), cnf_->id(), bufferLen) == 0) return first ? joinWorkers(true, errbuf) : MASTER;
          else return first ? joinWorkers(false, errbuf) : SLAVE;
        } else if (rc == ZNONODE) {
          break;
        } else if (rc != ZCONNECTIONLOSS) {
          if (errbuf) snprintf(errbuf, ERRBUF_MAX, "zoo_get %s error, %s", masterNode_.c_str(), zerror(rc));
          else log_fatal(0, "zoo_get %s error, %s", masterNode_.c_str(), zerror(rc));

          return ZKFATAL;
        }
        if (++i < ZKRETRY_MAX) millisleep(ZKRETRY_SLEEP);
      }
    } else {
      if (errbuf) snprintf(errbuf, ERRBUF_MAX, "zoo_create %s error, %s", masterNode_.c_str(), zerror(rc));
      else log_fatal(0, "zoo_create %s error, %s", masterNode_.c_str(), zerror(rc));

      return ZKFATAL;
    }
  } while (true);

  return ZKFATAL;
}

inline const char *zkTypeToString(int type)
{
  if (type == ZOO_CREATED_EVENT)     return "zoo_created_event";
  if (type == ZOO_DELETED_EVENT)     return "zoo_deleted_event";
  if (type == ZOO_CHANGED_EVENT)     return "zoo_changed_event";
  if (type == ZOO_CHILD_EVENT)       return "zoo_child_event";
  if (type == ZOO_SESSION_EVENT)     return "zoo_session_event";
  if (type == ZOO_NOTWATCHING_EVENT) return "zoo_notwatching_event";
  return 0;
}

inline const char *zkStateToString(int state)
{
  if (state == ZOO_EXPIRED_SESSION_STATE) return "zoo_expired_session_state";
  if (state == ZOO_AUTH_FAILED_STATE)     return "zoo_auth_failed_state";
  if (state == ZOO_CONNECTING_STATE)      return "zoo_connecting_state";
  if (state == ZOO_ASSOCIATING_STATE)     return "zoo_associating_state";
  if (state == ZOO_CONNECTED_STATE)       return "zoo_connected_state";
  return 0;
}

void ZkMgr::watchMasterNode(zhandle_t *, int type, int state, const char *path, void *watcherCtx)
{
  ZkMgr *mgr = (ZkMgr *) watcherCtx;

  if (type == ZOO_DELETED_EVENT || (type == ZOO_SESSION_EVENT && state == ZOO_EXPIRED_SESSION_STATE)) {
    pthread_mutex_lock(mgr->mutex_);
    if (type == ZOO_DELETED_EVENT) {
      mgr->zkStatus_ = MASTER_GONE;
    } else {
      mgr->zkStatus_ = SESSION_GONE;
    }
    pthread_mutex_unlock(mgr->mutex_);

    pthread_cond_signal(mgr->cond_);
  } else {
    log_error(0, "zk watch type %d, state %d, path %s", type, state, path ? path : "null");
  }
}

ZkMgr::NodeStatus ZkMgr::setWatch(char *errbuf)
{
  for (int i = 0; /**/; /**/) {
    int rc = zoo_wexists(zh_, masterNode_.c_str(), watchMasterNode, this, 0);
    if (rc == ZOK) {
      return ZKOK;
    } else if (rc == ZNONODE) {  // master had gone before set watch
      return ZKAGAIN;
    } else if (rc != ZCONNECTIONLOSS) {
      if (errbuf) snprintf(errbuf, ERRBUF_MAX, "zoo_wexists %s error, %s", masterNode_.c_str(), zerror(rc));
      else log_fatal(0, "zoo_wexists %s error, %s", masterNode_.c_str(), zerror(rc));

      return ZKFATAL;
    }
    if (++i < ZKRETRY_MAX) millisleep(ZKRETRY_SLEEP);
  }
  return ZKFATAL;
}

void ZkMgr::globalWatcher(zhandle_t *, int type, int state, const char *path, void *watcherCtx)
{
  const char *typeString  = zkTypeToString(type);
  const char *stateString = zkStateToString(state);

  if (typeString && stateString) {
    log_info(0, "globak zookeeper type %s(%d) state %s(%d) path %s",
             typeString, type, stateString, state, path ? path : "null");
  } else {
    log_info(0, "globak zookeeper type %d state %d path %s", type, state, path ? path : "null");
  }

  ZkMgr *mgr = (ZkMgr *) watcherCtx;
  if (type == ZOO_SESSION_EVENT && state == ZOO_EXPIRED_SESSION_STATE) {
    mgr->zkStatus_ = SESSION_GONE;
  }
}

inline zhandle_t *zookeeperInit(const char *zkhost, watcher_fn fn, void *ctx)
{
  for (int i = 0; /**/; /**/) {
    zhandle_t *zh = zookeeper_init(zkhost, fn, 15000, 0, ctx, 0);
    if (zh) return zh;
    if (errno != ZCONNECTIONLOSS) return 0;
    if (++i < ZKRETRY_MAX) millisleep(ZKRETRY_SLEEP);
  }
  return 0;
}

ZkMgr *ZkMgr::create(ConfigOpt *cnf, char *errbuf)
{
  std::auto_ptr<ZkMgr> mgr(new ZkMgr);
  mgr->cnf_ = cnf;
  mgr->fifoFd_ = -1;

  mgr->zkStatus_ = MASTER_GONE;
  mgr->mutex_    = &PTHREAD_MUTEX;
  mgr->cond_     = &PTHREAD_COND;

  zoo_set_debug_level(ZOO_LOG_LEVEL_ERROR);
  zoo_set_log_stream(stderr);

  mgr->zh_ = zookeeperInit(cnf->zkhost(), globalWatcher, mgr.get());
  if (!mgr->zh_) {
    snprintf(errbuf, ERRBUF_MAX, "%s zk connect %s error, %s", cnf->name(), cnf->zkhost(), zerror(errno));
    return 0;
  }

  if (!mgr->createWorkDir(errbuf)) return 0;

  bool stick = getStickFile(cnf->libdir(), cnf->name(), cnf->stick());
  do {
    if (stick || cnf->tcrash()) {
      if (cnf->zkdump()) dump_stick(cnf->id());

      mgr->status_ = mgr->competeMaster(true, errbuf);
    } else {
      millisleep(200 + random() % 999999);
      mgr->status_ = mgr->competeMaster(true, errbuf);
    }

    if (mgr->status_ == MASTER) {
      mgr->zkStatus_ = MASTER_WAIT;
    } else if (mgr->status_ == SLAVE) {
      if (cnf->retryStrategy() == ConfigOpt::RETRY_NOTHING) {
        mgr->status_ = OUT;
      } else {
        mgr->zkStatus_ = WORKER_SUSPEND;
        NodeStatus status = mgr->setWatch(errbuf);
        if (status != ZKOK) mgr->status_ = status;
      }
    }
  } while (mgr->status_ == ZKAGAIN);

  return mgr.release();
}

inline std::string join(size_t n, char *parray[])
{
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    if (i != 0) s.append(1, ' ');
    s.append(parray[i]);
  }
  return s;
}

void ZkMgr::setStatus(int exitStatus)
{
  Json::Value obj(Json::objectValue);
  obj["status"] = exitStatus;
  obj["id"] = cnf_->id();

  std::string json = Json::FastWriter().write(obj);
  if (json[json.size()-1] == '\n') json.resize(json.size()-1);

  log_info(0, "zoo_set status %s %s", statusNode_.c_str(), json.c_str());
  int rc = zoo_create(zh_, statusNode_.c_str(), json.c_str(), json.size(), &ZOO_DCRON_ALL_ACL, 0, 0, 0);
  if (rc == ZNODEEXISTS) {
    rc = zoo_set(zh_, statusNode_.c_str(), json.c_str(), json.size(), -1);
  }
  if (rc != ZOK) {
    log_fatal(0, "zoo_create/zoo_set %s error, %s", statusNode_.c_str(), zerror(rc));
  }
}

void ZkMgr::setResult(int retry, int exitStatus, const char *error)
{
  Json::Value obj(Json::objectValue);
  obj["status"] = exitStatus;
  obj["id"] = cnf_->id();
  obj["retry"] = retry;
  if (error) obj["error"] = error;

  std::string json = Json::FastWriter().write(obj);
  if (json[json.size()-1] == '\n') json.resize(json.size()-1);

  log_info(0, "zoo_set result %s%010d %s", resultNode_.c_str(), retry, json.c_str());
  int rc = zoo_create(zh_, resultNode_.c_str(), json.c_str(), json.size(), &ZOO_DCRON_ALL_ACL, ZOO_SEQUENCE, 0, 0);
  if (rc != ZOK) {
    log_fatal(errno, "zoo_create %s error, %s", resultNode_.c_str(), zerror(rc));
  }
}

#define MAX_ENVP_NUM 511
inline char * const *buildEnv(ConfigOpt *cnf, const std::map<std::string, std::string> &env)
{
  static char *envp[MAX_ENVP_NUM+1];
  int i = 0;

  static char fifoPtr[512];
  snprintf(fifoPtr, 512, "DCRON_FIFO=%s", cnf->fifo());
  envp[i++] = fifoPtr;

  for (std::map<std::string, std::string>::const_iterator ite = env.begin(); ite != env.end(); ++ite) {
    char *ptr = new char[4096];
    snprintf(ptr, 4096, "DCRON_%s=%s", ite->first.c_str(), ite->second.c_str());
    envp[i++] = ptr;
  }

  /* used for test */
  if (!ENV_STICK.empty()) envp[i++] = (char *) ENV_STICK.c_str();

  for (int j = 0; i < MAX_ENVP_NUM && environ[j]; ++j) envp[i++] = environ[j];
  envp[i++] = 0;
  return envp;
}

static bool getRomoteEnv(zhandle_t *zh, const char *path, std::map<std::string, std::string> *env)
{
  int bufferLen = RENV_BUFFER_LEN;
  std::auto_ptr<char> buffer(new char[RENV_BUFFER_LEN]);
  int rc = zoo_get(zh, path, 0, buffer.get(), &bufferLen, 0);
  if (rc != ZOK) {
    log_fatal(0, "zoo_get %s error, %s", path, zerror(rc));
    return false;
  }

  if (bufferLen <= 0) return true;

  Json::Value array;
  Json::Reader reader;
  if (!reader.parse(buffer.get(), buffer.get() + bufferLen, array)) {
    log_fatal(0, "%s content %s error", path, buffer.get());
    return false;
  }

  int len = array.size();
  for (int i = 0; i < len; ++i) {
    (*env)[array[i]["k"].asString()] = array[i]["v"].asString();
  }
  return true;
}

static bool setRemoteEnv(zhandle_t *zh, const char *path, std::map<std::string, std::string> *env)
{
  int bufferLen = RENV_BUFFER_LEN;
  std::auto_ptr<char> buffer(new char[RENV_BUFFER_LEN]);
  int rc = zoo_get(zh, path, 0, buffer.get(), &bufferLen, 0);
  if (rc != ZOK) {
    log_fatal(0, "zoo_get %s error, %s", path, zerror(rc));
    return false;
  }

  Json::Value array;
  if (bufferLen <= 0) {
    array = Json::Value(Json::arrayValue);
  } else {
    Json::Reader reader;
    if (!reader.parse(buffer.get(), buffer.get() + bufferLen, array)) {
      log_fatal(0, "%s content %s error", path, buffer.get());
      return false;
    }
  }

  for (size_t i = 0; i < array.size(); ++i) {
    const Json::Value &obj = array[(int) i];
    env->insert(std::make_pair(obj["k"].asString(), obj["v"].asString()));
  }
  while (env->size() > RENV_ITEM_MAX) env->erase(env->begin());

  array.resize(0);
  for (std::map<std::string, std::string>::iterator ite = env->begin(); ite != env->end(); ++ite) {
    Json::Value obj(Json::objectValue);
    obj["k"] = ite->first;
    obj["v"] = ite->second;
    array.append(obj);
  }

  std::string json = Json::FastWriter().write(array);
  if (json[json.size()-1] == '\n') json.resize(json.size()-1);

  log_info(0, "zoo_set llap %s %s", path, json.c_str());

  rc = zoo_set(zh, path, json.c_str(), json.size(), -1);
  if (rc != ZOK) {
    log_fatal(0, "zoo_set %s error, %s", path, zerror(rc));
    return false;
  } else {
    return true;
  }
}

inline bool setuid(const char *user, int uid, int gid)
{
  if (user && geteuid() == 0) {
    if (setgid(gid) == -1) return false;
    if (initgroups(user, gid) == -1) return false;
    if (setuid(uid) == -1) return false;
  }
  return true;
}

#define INTERNAL_ERROR_STATUS 254
pid_t ZkMgr::exec(int argc, char *argv[], const std::map<std::string, std::string> &env, int cnt)
{
  if (fifoFd_ > 0) close(fifoFd_);
  fifoFd_ = open(cnf_->fifo(), O_RDONLY | O_NONBLOCK);
  if (fifoFd_ == -1) {
    log_fatal(errno, "open fifo %s error", cnf_->fifo());
    setResult(cnt, INTERNAL_ERROR_STATUS, "fifo error");
    return -1;
  }

  pid_t pid = fork();
  if (pid == 0) {
    if (cnf_->captureStdio()) {
      std::string iof = cnf_->logdir() + "/" + cnf_->name() + ".stdout";
      int logFd = open(iof.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
      if (logFd != -1) dup2(logFd, STDOUT_FILENO);

      iof = cnf_->logdir() + "/" + cnf_->name() + ".stderr";
      logFd = open(iof.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
      if (logFd != -1) dup2(logFd, STDERR_FILENO);
    }

    if (!setuid(cnf_->user(), cnf_->uid(), cnf_->gid())) {
      log_fatal(errno, "setuid(%s) error", cnf_->user());
      exit(EXIT_FAILURE);
    }

    if (execve(argv[0], argv, buildEnv(cnf_, env)) == -1) {
      log_fatal(errno, "execve \"%s\" error", join(argc, argv).c_str());
      exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);  // never be exceuted here
  } else if (pid < 0) {
    log_fatal(errno, "fork error when exec %s", join(argc, argv).c_str());
    setResult(cnt, INTERNAL_ERROR_STATUS, "fork error");
    return -1;
  }

  return pid;
}

inline int getExitCode(int status)
{
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  else return status;
}

inline void deleteEmptyFile(const std::string &file)
{
  struct stat st;
  if (stat(file.c_str(), &st) == 0 && st.st_size == 0) {
    unlink(file.c_str());
  }
}

bool ZkMgr::wait(pid_t pid, size_t cnt, bool *retry, int *exitStatus)
{
  pid_t npid = waitpid(pid, exitStatus, WNOHANG);
  if (npid == -1) {
    log_fatal(errno, "%s waitpid error", cnf_->name());
    setResult(cnt, INTERNAL_ERROR_STATUS, "waitpid error");
    return true;
  } else if (npid == pid) {
    *exitStatus = getExitCode(*exitStatus);

    if (*exitStatus == 0 || cnf_->retryStrategy() == ConfigOpt::RETRY_NOTHING ||
        cnf_->retryStrategy() == ConfigOpt::RETRY_ON_CRASH) {
      setStatus(*exitStatus);
    } else if (cnf_->retryStrategy() == ConfigOpt::RETRY_ON_ABEXIT) {
      if (cnt+1 >= cnf_->maxRetry()) {
        setStatus(*exitStatus);
      } else {
        setResult(cnt, *exitStatus);
        *retry = true;
      }
    }

    if (cnf_->captureStdio()) {
      deleteEmptyFile(cnf_->logdir() + "/" + cnf_->name() + ".stdout");
      deleteEmptyFile(cnf_->logdir() + "/" + cnf_->name() + ".stderr");
    }

    if (cnf_->stick()) createStickFile(cnf_->libdir(), cnf_->name());
    return true;
  } else {
    return false;
  }
}

void ZkMgr::rsyncFifoData()
{
  char buffer[PIPE_BUF];
  ssize_t nn;
  std::map<std::string, std::string> env;
  while ((nn = read(fifoFd_, buffer, PIPE_BUF)) > 0) {
    int start = 0;
    int eq = -1;
    for (int i = 0; i < nn; ++i) {
      if (buffer[i] == '=') {
        eq = i;
      } else if (buffer[i] == '\n' && eq != -1) {
        env[std::string(buffer + start, eq - start)] = std::string(buffer + eq+1, i - (eq+1));
        eq = -1;
        start = i + 1;
      }
      if (env.size() > RENV_ITEM_MAX) env.erase(env.begin());
    }
  }

  if (nn == -1 && errno != EAGAIN) {
    log_fatal(errno, "%s fifo %s read error", cnf_->name(), cnf_->fifo());
  }

  if (!env.empty()) setRemoteEnv(zh_, llapNode_.c_str(), &env);
}

int ZkMgr::exec(int argc, char *argv[])
{
  if (cnf_->tcrash()) abort();

  std::map<std::string, std::string> env;
  if (!getRomoteEnv(zh_, llapNode_.c_str(), &env)) {
    setResult(0, INTERNAL_ERROR_STATUS, "zk error");
    return INTERNAL_ERROR_STATUS;
  }

  if (mkfifo(cnf_->fifo(), 0644) != 0 && errno != EEXIST) {
    log_fatal(errno, "mkfifo %s error", cnf_->fifo());
    setResult(0, INTERNAL_ERROR_STATUS, "mkfifo error");
    return INTERNAL_ERROR_STATUS;
  }

  if (cnf_->user() && chown(cnf_->fifo(), cnf_->uid(), cnf_->gid()) == -1) {
    log_fatal(errno, "chown %s error", cnf_->fifo());
    setResult(0, INTERNAL_ERROR_STATUS, "chown error");
    return INTERNAL_ERROR_STATUS;
  }

  bool retry = true;
  int exitStatus;
  for (int cnt = 0; retry; ++cnt) {
    retry = false;

    pid_t pid = exec(argc, argv, env, cnt);
    if (pid < 0) {
      exitStatus = INTERNAL_ERROR_STATUS;
      break;
    }

    do {
      bool childExit = wait(pid, cnt, &retry, &exitStatus);
      rsyncFifoData();

      if (childExit) {
        break;
      } else if (zkStatus_ == SESSION_GONE) {  // session expired
        kill(pid, SIGTERM);
        log_error(0, "zookeeper session expired, had lost master, exit");
        break;
      } else {
        millisleep(10);
      }
    } while (true);
  }

  unlink(cnf_->fifo());
  return exitStatus;
}

inline bool zooGetJson(zhandle_t *zh, const char *node, char *buffer, Json::Value *root)
{
  int bufferLen = RENV_BUFFER_LEN;
  int rc = zoo_get(zh, node, 0, buffer, &bufferLen, 0);
  if (rc != ZOK && rc != ZNONODE) {
    log_fatal(0, "zoo_get %s error, %s", node, zerror(rc));
    return false;
  }

  if (rc == ZOK && bufferLen > 0) {
    Json::Reader reader;
    if (!reader.parse(buffer, buffer + bufferLen, *root)) return false;
  }
  return true;
}

void ZkMgr::suspend()
{
  log_info(0, "%s %s suspend", cnf_->id(), cnf_->name());

  pthread_mutex_lock(mutex_);
  if (zkStatus_ == WORKER_SUSPEND) pthread_cond_wait(cond_, mutex_);
  pthread_mutex_unlock(mutex_);

  if (zkStatus_ == SESSION_GONE) return;  // session expired

  log_info(0, "%s %s wake up", cnf_->id(), cnf_->name());

  if (!cnf_->llap()) {
    int bufferLen = RENV_BUFFER_LEN;
    std::auto_ptr<char> buffer(new char[bufferLen]);
    int rc = zoo_get(zh_, statusNode_.c_str(), 0, buffer.get(), &bufferLen, 0);

    if (cnf_->retryStrategy() == ConfigOpt::RETRY_ON_ABEXIT) {
      if (rc == ZOK) status_ = OUT;
      else if (rc != ZNONODE) status_ = ZKFATAL;
    } else {   // ConfigOpt::RETRY_ON_CRASH
      if (rc == ZOK) {
        Json::Reader reader;
        Json::Value  root;
        if (bufferLen > 0 && reader.parse(buffer.get(), buffer.get() + bufferLen, root)) {
          if (root["status"].asInt() == 0) status_ = OUT;
        }
      } else if (rc != ZNONODE) {
        status_ = ZKFATAL;
      }
    }

    if (status_ == ZKFATAL) {
      log_fatal(0, "zoo_get %s error, %s", statusNode_.c_str(), zerror(rc));
    }
  }

  if (status_ == SLAVE) {
    log_info(0, "%s %s run", cnf_->id(), cnf_->name());
    do {
      status_ = competeMaster(false, 0);
      if (status_ == MASTER) {
        zkStatus_ = MASTER_WAIT;
      } else if (status_ == SLAVE) {
        zkStatus_ = WORKER_SUSPEND;

        NodeStatus stat = setWatch(0);
        if (stat != ZKOK) status_ = stat;
      }
    } while (status_ == ZKAGAIN);
  }
}

bool ZkMgr::dump(std::string *json) const
{
  Json::Value obj(Json::objectValue);
  std::auto_ptr<char> buffer(new char[RENV_BUFFER_LEN]);

  Json::Value root = Json::Value(Json::objectValue);
  if (zooGetJson(zh_, llapNode_.c_str(), buffer.get(), &root)) obj["llap"] = root;

  root = Json::Value(Json::arrayValue);
  if (zooGetJson(zh_, workersNode_.c_str(), buffer.get(), &root)) obj["workers"] = root;

  root = Json::nullValue;
  zooGetJson(zh_, statusNode_.c_str(), buffer.get(), &root);
  obj["status"] = root;

  Json::Value array(Json::arrayValue);
  for (int i = 0; i < 10; ++i) {
    root = Json::nullValue;

    char result[32];
    snprintf(result, 32, "/result%010d", i);
    std::string resultPath = taskPath_ + result;

    if (zooGetJson(zh_, resultPath.c_str(), buffer.get(), &root) && !root.isNull()) array.append(root);
  }
  obj["result"] = array;

  obj["taskPath"]    = taskPath_;
  obj["statusNode"]  = statusNode_;
  obj["workersNode"] = workersNode_;
  obj["llapNode"]    = llapNode_;

  json->assign(Json::FastWriter().write(obj));
  return true;
}
