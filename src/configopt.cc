#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "configopt.h"

inline bool getenv(const char *name, std::string *value)
{
  const char *ptr = getenv(name);
  if (!ptr) return false;

  value->assign(ptr);
  return true;
}

inline void getenv(const char *name, std::string *value, const std::string &def)
{
  const char *ptr = getenv(name);

  if (ptr) value->assign(ptr);
  else value->assign(def);
}

template <class IntType>
bool getenv(const char *name, IntType *value, IntType def)
{
  const char *ptr = getenv(name);
  if (ptr) {
    char *endptr;
    long int intval = strtol(ptr, &endptr, 10);
    if (*endptr != '\0') return false;
    *value = intval;
  } else {
    *value = def;
  }
  return true;
}

inline bool getenv(const char *name, bool *b, bool def)
{
  const char *ptr = getenv(name);
  if (ptr) {
    if (strcmp(ptr, "true") == 0 || strcmp(ptr, "1") == 0) {
      *b = true;
    } else if (strcmp(ptr, "false") == 0 || strcmp(ptr, "0") == 0) {
      *b = false;
    } else {
      return false;
    }
  } else {
    *b = def;
  }
  return true;
}

inline bool getIpByEth(const char *eth, std::string *ip)
{
  struct ifreq ifr;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == -1) return false;

  ifr.ifr_addr.sa_family = AF_INET;
  strncpy(ifr.ifr_name, eth, IFNAMSIZ-1);

  if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
    close(fd);
    return false;
  }

  close(fd);

  char buffer[16];
  if (!inet_ntop(AF_INET, & ((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr, buffer, 16)) return false;

  ip->append(buffer);
  return true;
}

#define ERRBUF_MAX 256
ConfigOpt *ConfigOpt::create(char *errbuf)
{
  std::auto_ptr<ConfigOpt> opt(new ConfigOpt);
  std::string str;

  if (!getenv("DCRON_ID", &opt->id_) && !getIpByEth("eth0", &opt->id_)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_ID get default eth0 ip failed, %s", strerror(errno));
    return 0;
  }

  if (!getenv("DCRON_ZK", &opt->zkhost_)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_ZK is required");
    return 0;
  }

  if (!getenv("DCRON_MAXRETRY", &opt->maxRetry_, 2)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_MAXRETRY is not a number");
    return 0;
  }
  if (opt->maxRetry_ > 5) opt->maxRetry_ = 5;

  getenv("DCRON_RETRYON", &str, "CRASH");
  if (str == "CRASH") {
    opt->retryStrategy_ = RETRY_ON_CRASH;
  } else if (str == "ABEXIT") {
    opt->retryStrategy_ = RETRY_ON_ABEXIT;
  } else {
    opt->retryStrategy_ = RETRY_NOTHING;
  }

  if (!getenv("DCRON_LLAP", &opt->llap_, false)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_LLAP is not a boolean");
    return 0;
  }

  if (!getenv("DCRON_STICK", &opt->stick_, opt->llap_)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_STICK is not a boolean");
    return 0;
  }

  if (!getenv("DCRON_STDIOCAP", &opt->captureStdio_, !opt->llap_)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_STDIOCAP is not a boolean");
    return 0;
  }

  getenv("DCRON_LIBDIR", &opt->libdir_, "/var/lib/dcron");
  getenv("DCRON_LOGDIR", &opt->logdir_, "/var/log/dcron");

  if (!getenv("DCRON_NAME", &str)) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_NAME is required");
    return 0;
  }

  if (opt->llap_) str.append(".%Y%m%d_%H%M");

  time_t now = time(0);
  struct tm ltm;
  localtime_r(&now, &ltm);
  char buffer[128];
  if (!strftime(buffer, 128, str.c_str(), &ltm)) {
    snprintf(errbuf, ERRBUF_MAX, "strftime %s error", str.c_str());
    return 0;
  }

  if (str == buffer) {
    snprintf(errbuf, ERRBUF_MAX, "ENV DCRON_NAME must contain taskid, like .%%Y%%m%%d_%%H%%M");
    return 0;
  }
  opt->name_.assign(buffer);

  opt->fifo_ = opt->libdir_ + "/" + opt->name_ + ".fifo";
  getenv("DCRON_ZKDUMP", &opt->zkdump_);

  return opt.release();
}
